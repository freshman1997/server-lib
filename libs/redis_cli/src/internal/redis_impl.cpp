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
        completion_event_.notify();
    }

    void RedisClient::Impl::close()
    {
        if (!conn_ && is_closed()) {
            return;
        }

        set_mask(RedisState::closed, true);
        if (conn_) {
            conn_->close();
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
            last_error_ = ErrorValue::from_string("response buffer exceeds limit");
            close();
            return;
        }

        const auto cmd = last_cmd_ ? last_cmd_ : subcribe_cmd;
        if (const int ret = cmd->unpack(reader_); ret < 0) {
            if (ret == UnpackCode::need_more_bytes) {
                return;
            } else if (ret == UnpackCode::format_error) {
                last_error_ = ErrorValue::from_string("format error");
            } else {
                last_error_ = ErrorValue::from_string("unknown error");
            }
        } else {
            if (cmd->get_result() && cmd->get_result()->get_type() == resp_error) {
                last_error_ = cmd->get_result();
                cmd->set_result(nullptr);
            }
        }

        if (!last_cmd_ && subcribe_cmd) {
            if (subcribe_cmd->get_result()) {
                if (!subcribe_cmd->is_subcribe()) {
                    subcribe_cmd = nullptr;
                }
            } else if (is_subscription_parse_failure(subcribe_cmd)) {
                last_error_ = ErrorValue::from_string("subscribe failed");
            }
        }

        if (reader_.get_remain_bytes() > 0) {
            reader_.discard_read_bytes();
        } else {
            reader_.just_clear();
        }
        if (!last_cmd_ || cmd->get_result() || last_error_ ||
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
        completion_event_.notify();
    }

    std::shared_ptr<RedisValue> RedisClient::Impl::execute_command(std::shared_ptr<Command> cmd)
    {
        std::lock_guard<std::recursive_mutex> lock(operation_mutex_);

        if (!cmd) {
            last_error_ = ErrorValue::from_string("command is null");
            return nullptr;
        }

        if (is_closed()) {
            return nullptr;
        }

        if (multi_cmd_) {
            multi_cmd_->add_command(cmd);
            return nullptr;
        }

        if (!is_connecting() && !is_connected()) {
            if (!client_) {
                last_error_ = ErrorValue::from_string("unexpected error");
                return nullptr;
            }

            if (const int ret = client_->connect(); ret < 0) {
                if (!last_error_) {
                    last_error_ = ErrorValue::from_string(option_.name_ + " connect failed");
                }
                return nullptr;
            }
        }

        if (!is_connected()) {
            last_error_ = ErrorValue::from_string("not connected");
            return nullptr;
        }

        if (last_cmd_) {
            last_error_ = ErrorValue::from_string("executing");
            return nullptr;
        }

        last_error_ = nullptr;

        last_cmd_ = cmd;
        completion_event_.reset(RedisRegistry::get_instance()->get_event_loop());

        if (!conn_) {
            last_error_ = ErrorValue::from_string("connection missing");
            last_cmd_ = nullptr;
            return nullptr;
        }

        const auto &cmdStr = cmd->pack();
        conn_->write(::yuan::buffer::ByteBuffer(std::string_view(cmdStr.data(), cmdStr.size())));

        const auto runtime = RedisRegistry::get_instance()->get_coroutine_runtime();
        auto res = yuan::coroutine::sync_wait(runtime, [this, cmd]()->SimpleTask<std::shared_ptr<RedisValue> > {
            const auto timeout_ms = option_.timeout_ms_ > 0 ? static_cast<uint32_t>(option_.timeout_ms_) : 0;
            const bool timed_out = co_await completion_event_.wait_for(
                RedisRegistry::get_instance()->get_timer_manager(),
                timeout_ms);
            if (timed_out) {
                last_error_ = ErrorValue::from_string(option_.name_ + " command timeout");
                close();
                co_return nullptr;
            }
            if (client_ && client_->is_closed()) {
                client_->set_last_error(ErrorValue::from_string("connection closed"));
            }
            co_return cmd->get_result(); }());

        last_cmd_ = nullptr;

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
            last_error_ = ErrorValue::from_string("connection closed");
            return -1;
        }

        if (!is_connected()) {
            last_error_ = ErrorValue::from_string("not connected");
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
            const auto runtime = RedisRegistry::get_instance()->get_coroutine_runtime();
            completion_event_.reset(RedisRegistry::get_instance()->get_event_loop());
            const bool is_timeout = co_await completion_event_.wait_for(
                RedisRegistry::get_instance()->get_timer_manager(),
                timeout > 0 ? static_cast<uint32_t>(timeout) : 0);

            if (is_timeout) {
                last_error_ = ErrorValue::from_string("fetch message timeout");
            }

            co_return is_timeout ? -1 : 0;
        };
        const auto runtime = RedisRegistry::get_instance()->get_coroutine_runtime();
        const auto result = yuan::coroutine::sync_wait(runtime, wait_task());

        if (result == 0 && subcribe_cmd && subcribe_cmd->has_pending_messages()) {
            subcribe_cmd->exec_callback();
        }

        return result;
    }
}
