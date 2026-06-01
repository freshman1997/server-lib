#include "redis_impl.h"
#include "buffer/byte_buffer.h"
#include "coroutine.h"
#include "coroutine/sync_wait.h"
#include "def.h"
#include "event/event_loop.h"
#include "internal/redis_registry.h"
#include "timer/timer_manager.h"
#include "value/error_value.h"

#include <cassert>
#include <chrono>
#include <thread>

namespace yuan::redis
{
    namespace
    {
        bool is_subscription_parse_failure(const std::shared_ptr<SubcribeCmd> &cmd)
        {
            return cmd && !cmd->get_result() && !cmd->has_pending_messages();
        }
    }

    RedisClient::Impl::~Impl()
    {
        close();
        client_ = nullptr;
    }

    RedisRegistry *RedisClient::Impl::registry() const
    {
        return registry_ ? registry_.get() : RedisRegistry::get_instance().get();
    }

    void RedisClient::Impl::on_connected(const std::shared_ptr<net::Connection> &conn)
    {
        conn_ = conn;
        set_mask(RedisState::connected);
        completion_event_.notify();
    }

    void RedisClient::Impl::on_error(const std::shared_ptr<net::Connection> &conn)
    {
        (void)conn;
        conn_.reset();
        set_mask(RedisState::closed, true);
        last_error_.store(ErrorValue::from_string("connection error"));
        multi_cmd_ = nullptr;
        subcribe_cmd = nullptr;
        completion_event_.notify();
    }

    void RedisClient::Impl::close()
    {
        if (!conn_ && is_closed()) {
            return;
        }

        closed_by_user_.store(true, std::memory_order_release);
        set_mask(RedisState::closed, true);
        if (conn_) {
            conn_->set_connection_handler(nullptr);
            conn_->close();
            registry()->drain_pending();
            conn_.reset();
        }
        last_cmd_ = nullptr;
        reader_.just_clear();
        completion_event_.notify();
    }

    void RedisClient::Impl::disconnect()
    {
        if (!conn_ && is_closed()) {
            return;
        }

        closed_by_user_.store(false, std::memory_order_release);
        set_mask(RedisState::closed, true);
        if (conn_) {
            conn_->set_connection_handler(nullptr);
            conn_->close();
            registry()->drain_pending();
            conn_.reset();
        }
        last_cmd_ = nullptr;
        reader_.just_clear();
        completion_event_.notify();
    }

    void RedisClient::Impl::on_read(const std::shared_ptr<net::Connection> &conn)
    {
        if (!conn) {
            return;
        }
        if ((!last_cmd_ && !subcribe_cmd) || !is_connected()) {
            return;
        }

        reader_.add_buffer(conn->take_input_byte_buffer());
        if (option_.max_buffered_response_bytes_ > 0 &&
            reader_.get_remain_bytes() > option_.max_buffered_response_bytes_) {
            last_error_.store(ErrorValue::from_string("response buffer exceeds limit"));
            if (conn_) {
                conn_->set_connection_handler(nullptr);
                conn_->close();
                conn_.reset();
            }
            set_mask(RedisState::closed, true);
            last_cmd_ = nullptr;
            multi_cmd_ = nullptr;
            subcribe_cmd = nullptr;
            completion_event_.notify();
            return;
        }

        const auto cmd = last_cmd_ ? last_cmd_ : subcribe_cmd;
        if (const int ret = cmd->unpack(reader_); ret < 0) {
            if (ret == UnpackCode::need_more_bytes) {
                return;
            } else if (ret == UnpackCode::format_error) {
                last_error_.store(ErrorValue::from_string("format error"));
            } else {
                last_error_.store(ErrorValue::from_string("unknown error"));
            }
        } else {
            if (cmd->get_result() && cmd->get_result()->get_type() == resp_error) {
                last_error_.store(cmd->get_result());
                cmd->set_result(nullptr);
            }
        }

        if (!last_cmd_ && subcribe_cmd) {
            if (subcribe_cmd->get_result()) {
                if (!subcribe_cmd->is_subcribe()) {
                    subcribe_cmd = nullptr;
                }
            } else if (is_subscription_parse_failure(subcribe_cmd)) {
                last_error_.store(ErrorValue::from_string("subscribe failed"));
            }
        }

        if (reader_.get_remain_bytes() > 0) {
            reader_.discard_read_bytes();
        } else {
            reader_.just_clear();
        }
        if (!last_cmd_ || cmd->get_result() || last_error_.load() ||
            (subcribe_cmd && subcribe_cmd->has_pending_messages())) {
            completion_event_.notify();
        }
    }

    void RedisClient::Impl::on_write(const std::shared_ptr<net::Connection> &conn)
    {
        (void)conn;
    }

    void RedisClient::Impl::on_close(const std::shared_ptr<net::Connection> &conn)
    {
        (void)conn;
        conn_.reset();
        set_mask(RedisState::closed, true);
        last_error_.store(ErrorValue::from_string("connection closed by remote"));
        multi_cmd_ = nullptr;
        subcribe_cmd = nullptr;
        completion_event_.notify();
    }

    std::shared_ptr<RedisValue> RedisClient::Impl::execute_command(std::shared_ptr<Command> cmd)
    {
        std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
        in_flight_.fetch_add(1, std::memory_order_relaxed);

        if (!cmd) {
            last_error_.store(ErrorValue::from_string("command is null"));
            in_flight_.fetch_sub(1, std::memory_order_relaxed);
            return nullptr;
        }

        if (is_closed()) {
            if (closed_by_user_.load(std::memory_order_acquire)) {
                in_flight_.fetch_sub(1, std::memory_order_relaxed);
                return nullptr;
            }
            if (!option_.reconnect_ || !client_) {
                in_flight_.fetch_sub(1, std::memory_order_relaxed);
                return nullptr;
            }

            closed_by_user_.store(false, std::memory_order_release);
            bool reconnected = false;
            for (int i = 0; i < option_.max_reconnect_retries_; ++i) {
                if (i > 0 && option_.reconnect_delay_ms_ > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(option_.reconnect_delay_ms_));
                }
                if (client_->connect() == 0) {
                    reconnected = true;
                    break;
                }
            }

            if (!reconnected) {
                last_error_.store(ErrorValue::from_string(option_.name_ + " reconnect failed after " +
                    std::to_string(option_.max_reconnect_retries_) + " retries"));
                in_flight_.fetch_sub(1, std::memory_order_relaxed);
                return nullptr;
            }
        }

        if (multi_cmd_) {
            multi_cmd_->add_command(cmd);
            in_flight_.fetch_sub(1, std::memory_order_relaxed);
            return nullptr;
        }

        if (!is_connecting() && !is_connected()) {
            if (!client_) {
                last_error_.store(ErrorValue::from_string("unexpected error"));
                in_flight_.fetch_sub(1, std::memory_order_relaxed);
                return nullptr;
            }

            if (const int ret = client_->connect(); ret < 0) {
                if (!last_error_.load()) {
                    last_error_.store(ErrorValue::from_string(option_.name_ + " connect failed"));
                }
                in_flight_.fetch_sub(1, std::memory_order_relaxed);
                return nullptr;
            }
        }

        if (!is_connected()) {
            last_error_.store(ErrorValue::from_string("not connected"));
            in_flight_.fetch_sub(1, std::memory_order_relaxed);
            return nullptr;
        }

        if (last_cmd_) {
            last_error_.store(ErrorValue::from_string("executing"));
            in_flight_.fetch_sub(1, std::memory_order_relaxed);
            return nullptr;
        }

        last_error_.store(nullptr);

        last_cmd_ = cmd;
        completion_event_.reset(registry()->get_event_loop());

        if (!conn_) {
            last_error_.store(ErrorValue::from_string("connection missing"));
            last_cmd_ = nullptr;
            in_flight_.fetch_sub(1, std::memory_order_relaxed);
            return nullptr;
        }

        const auto runtime = registry()->get_coroutine_runtime();
        std::shared_ptr<RedisValue> res;
        try {
            res = yuan::coroutine::sync_wait_locked(runtime, [this, cmd]()->SimpleTask<std::shared_ptr<RedisValue> > {
            const auto &cmdStr = cmd->pack();
            conn_->write(::yuan::buffer::ByteBuffer(std::string_view(cmdStr.data(), cmdStr.size())));
            const int ct = option_.command_timeout_ms_ > 0 ? option_.command_timeout_ms_
                : (option_.timeout_ms_ > 0 ? option_.timeout_ms_ : 0);
            const auto timeout_ms = ct > 0 ? static_cast<uint32_t>(ct) : 0;
            const bool timed_out = co_await completion_event_.wait_for(
                registry()->get_timer_manager(),
                timeout_ms);
            if (timed_out) {
                last_error_.store(ErrorValue::from_string(option_.name_ + " command timeout"));
                close();
                co_return nullptr;
            }
            if (client_ && client_->is_closed()) {
                client_->set_last_error(ErrorValue::from_string("connection closed"));
            }
            co_return cmd->get_result(); }(), "redis event loop is already running on another thread");
        } catch (const std::exception &ex) {
            last_error_.store(ErrorValue::from_string(ex.what()));
            last_cmd_ = nullptr;
            disconnect();
            in_flight_.fetch_sub(1, std::memory_order_relaxed);
            return nullptr;
        }

        last_cmd_ = nullptr;
        in_flight_.fetch_sub(1, std::memory_order_relaxed);

        return res;
    }

    void RedisClient::Impl::on_do_connect(std::shared_ptr<net::Connection> conn)
    {
        clear_mask();
        set_mask(RedisState::connecting);
        conn_ = std::move(conn);

    }

    int RedisClient::Impl::fetch_next_message(int timeout)
    {
        std::lock_guard<std::recursive_mutex> lock(operation_mutex_);

        if (is_closed()) {
            last_error_.store(ErrorValue::from_string("connection closed"));
            return -1;
        }

        if (!is_connected()) {
            last_error_.store(ErrorValue::from_string("not connected"));
            return -1;
        }

        if (subcribe_cmd && subcribe_cmd->has_pending_messages()) {
            subcribe_cmd->exec_callback();
            return 0;
        }

        const auto wait_task = [
            timeout,
            this
        ]()->SimpleTask<int>
        {
            const auto runtime = registry()->get_coroutine_runtime();
            completion_event_.reset(registry()->get_event_loop());
            const bool is_timeout = co_await completion_event_.wait_for(
                registry()->get_timer_manager(),
                timeout > 0 ? static_cast<uint32_t>(timeout) : 0);

            if (is_timeout) {
                last_error_.store(ErrorValue::from_string("fetch message timeout"));
            }

            co_return is_timeout ? -1 : 0;
        };
        const auto runtime = registry()->get_coroutine_runtime();
        int result = -1;
        try {
            result = yuan::coroutine::sync_wait_locked(
                runtime,
                wait_task(),
                "redis event loop is already running on another thread");
        } catch (const std::exception &ex) {
            last_error_.store(ErrorValue::from_string(ex.what()));
            return -1;
        }

        if (result == 0 && subcribe_cmd && subcribe_cmd->has_pending_messages()) {
            subcribe_cmd->exec_callback();
        }

        return result;
    }
}
