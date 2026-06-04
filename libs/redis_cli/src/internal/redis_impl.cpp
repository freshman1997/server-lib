#include "redis_impl.h"
#include "buffer/byte_buffer.h"
#include "coroutine.h"
#include "coroutine/sync_wait.h"
#include "def.h"
#include "event/event_loop.h"
#include "internal/redis_registry.h"
#include "timer/timer_manager.h"
#include "value/error_value.h"
#include "base/time.h"

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

        class InFlightGuard
        {
        public:
            explicit InFlightGuard(std::atomic<int> &counter, std::condition_variable &cv) noexcept
                : counter_(counter), cv_(cv)
            {
                counter_.fetch_add(1, std::memory_order_acq_rel);
            }

            ~InFlightGuard()
            {
                counter_.fetch_sub(1, std::memory_order_acq_rel);
                cv_.notify_all();
            }

            InFlightGuard(const InFlightGuard &) = delete;
            InFlightGuard &operator=(const InFlightGuard &) = delete;

        private:
            std::atomic<int> &counter_;
            std::condition_variable &cv_;
        };
    }

    RedisClient::Impl::~Impl()
    {
        close();
        client_ = nullptr;
    }

    std::shared_ptr<RedisRegistry> RedisClient::Impl::registry() const
    {
        return registry_ ? registry_ : RedisRegistry::get_instance();
    }

    void RedisClient::Impl::on_connected(net::Connection &conn)
    {
        if (is_closed()) {
            conn.set_connection_handler(nullptr);
            conn.close();
            return;
        }
        conn_ = conn.shared_from_this();
        set_mask(RedisState::connected);
        completion_event_.notify();
    }

    void RedisClient::Impl::on_error(net::Connection &conn)
    {
        (void)conn;
        if (is_closed()) return;
        conn_.reset();
        set_mask(RedisState::closed, true);
        last_error_.store(ErrorValue::from_string("connection error"));
        multi_cmd_ = nullptr;
        subcribe_cmd = nullptr;
        completion_event_.notify();
    }

    void RedisClient::Impl::close()
    {
        std::lock_guard<std::recursive_mutex> lock(operation_mutex_);

        if (!conn_ && is_closed()) {
            return;
        }

        health_check_closing_.store(true, std::memory_order_release);
        health_check_cv_.notify_all();
        if (health_check_thread_.joinable()) {
            health_check_thread_.join();
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
        multi_cmd_ = nullptr;
        subcribe_cmd = nullptr;
        reader_.just_clear();
        completion_event_.notify();
    }

    void RedisClient::Impl::disconnect()
    {
        std::lock_guard<std::recursive_mutex> lock(operation_mutex_);

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
        multi_cmd_ = nullptr;
        subcribe_cmd = nullptr;
        reader_.just_clear();
        completion_event_.notify();
    }

    void RedisClient::Impl::on_read(net::Connection &conn)
    {
        reader_.add_buffer(conn.take_input_byte_buffer());

        if (!is_connected()) {
            reader_.just_clear();
            return;
        }

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

        if (!last_cmd_ && !subcribe_cmd) {
            if (reader_.get_remain_bytes() > 0) {
                reader_.just_clear();
            }
            return;
        }

        const auto cmd = last_cmd_ ? last_cmd_ : subcribe_cmd;
        bool protocol_error = false;
        if (const int ret = cmd->unpack(reader_); ret < 0) {
            if (ret == UnpackCode::need_more_bytes) {
                return;
            } else if (ret == UnpackCode::format_error) {
                last_error_.store(ErrorValue::from_string("format error"));
                protocol_error = true;
            } else {
                last_error_.store(ErrorValue::from_string("unknown error"));
                protocol_error = true;
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
                protocol_error = true;
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

        if (protocol_error) {
            protocol_errors_.fetch_add(1, std::memory_order_release);
            disconnect();
        }
    }

    void RedisClient::Impl::on_write(net::Connection &conn)
    {
        (void)conn;
    }

    void RedisClient::Impl::on_close(net::Connection &conn)
    {
        (void)conn;
        if (is_closed()) return;
        conn_.reset();
        set_mask(RedisState::closed, true);
        last_error_.store(ErrorValue::from_string("connection closed by remote"));
        multi_cmd_ = nullptr;
        subcribe_cmd = nullptr;
        completion_event_.notify();
    }

    void RedisClient::Impl::on_input_shutdown(net::Connection &conn)
    {
        (void)conn;
    }

std::shared_ptr<RedisValue> RedisClient::Impl::execute_command(std::shared_ptr<Command> cmd, int override_timeout_ms)
{
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    InFlightGuard in_flight_guard(in_flight_, in_flight_cv_);

    if (!cmd) {
        last_error_.store(ErrorValue::from_string("command is null"));
        return nullptr;
    }

    commands_total_.fetch_add(1, std::memory_order_release);
    const auto start_us = yuan::base::time::steady_now_us();

    if (is_closed()) {
        if (closed_by_user_.load(std::memory_order_acquire)) {
            return nullptr;
        }
        if (!option_.reconnect_ || !client_) {
            return nullptr;
        }

        closed_by_user_.store(false, std::memory_order_release);
        bool reconnected = false;
        for (int i = 0; i < option_.max_reconnect_retries_; ++i) {
            reconnect_attempts_.fetch_add(1, std::memory_order_release);
            if (i > 0 && option_.reconnect_delay_ms_ > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(option_.reconnect_delay_ms_));
            }
            if (client_->connect() == 0) {
                reconnect_successes_.fetch_add(1, std::memory_order_release);
                reconnected = true;
                break;
            }
        }

        if (!reconnected) {
            last_error_.store(ErrorValue::from_string(option_.name_ + " reconnect failed after " +
                std::to_string(option_.max_reconnect_retries_) + " retries"));
            return nullptr;
        }
    }

    if (multi_cmd_) {
        multi_cmd_->add_command(cmd);
        return nullptr;
    }

    if (!is_connecting() && !is_connected()) {
        if (!client_) {
            last_error_.store(ErrorValue::from_string("unexpected error"));
            return nullptr;
        }

        if (const int ret = client_->connect(); ret < 0) {
            if (!last_error_.load()) {
                last_error_.store(ErrorValue::from_string(option_.name_ + " connect failed"));
            }
            return nullptr;
        }
    }

    if (!is_connected()) {
        last_error_.store(ErrorValue::from_string("not connected"));
        return nullptr;
    }

    if (last_cmd_) {
        last_error_.store(ErrorValue::from_string("executing"));
        return nullptr;
    }

    last_error_.store(nullptr);

    last_cmd_ = cmd;
    completion_event_.reset(registry()->get_event_loop());

    if (!conn_) {
        last_error_.store(ErrorValue::from_string("connection missing"));
        last_cmd_ = nullptr;
        return nullptr;
    }

    const auto runtime = registry()->get_coroutine_runtime();
    const int ct = override_timeout_ms >= 0 ? override_timeout_ms
        : (option_.command_timeout_ms_ > 0 ? option_.command_timeout_ms_
        : (option_.timeout_ms_ > 0 ? option_.timeout_ms_ : 0));
    const auto timeout_ms = ct > 0 ? static_cast<uint32_t>(ct) : 0;
    std::shared_ptr<RedisValue> res;
    try {
        res = yuan::coroutine::sync_wait_locked(runtime, [this, cmd, timeout_ms]()->SimpleTask<std::shared_ptr<RedisValue> > {
        const auto &cmdStr = cmd->pack();
        conn_->write(::yuan::buffer::ByteBuffer(std::string_view(cmdStr.data(), cmdStr.size())));
        const bool timed_out = co_await completion_event_.wait_for(
            registry()->get_timer_manager(),
            timeout_ms);
        if (timed_out) {
            command_timeouts_.fetch_add(1, std::memory_order_release);
            last_error_.store(ErrorValue::from_string(option_.name_ + " command timeout"));
            disconnect();
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
        return nullptr;
    }

    last_cmd_ = nullptr;

    const auto elapsed_us = yuan::base::time::steady_now_us() - start_us;
    total_latency_us_.fetch_add(elapsed_us, std::memory_order_release);

    if (!res || (res && res->get_type() == resp_error)) {
        command_errors_.fetch_add(1, std::memory_order_release);
    }

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
