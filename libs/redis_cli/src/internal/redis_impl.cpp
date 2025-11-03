#include "redis_impl.h"
#include "buffer/buffer.h"
#include "coroutine.h"
#include "def.h"
#include "event/event_loop.h"
#include "internal/command_manager.h"
#include "internal/redis_registry.h"
#include "value/error_value.h"
#include "value/string_value.h"
#include "buffer/linked_buffer.h"

namespace yuan::redis 
{
    static inline void resume()
    {
        RedisRegistry::get_instance()->get_event_loop()->set_use_coroutine(true);
    }

    void RedisClient::Impl::on_connected(net::Connection *conn)
    {
        if (!option_.password_.empty())
        {
            auto cmd = std::make_shared<DefaultCmd>();
            cmd->set_args("auth", { StringValue::from_string(option_.password_)});
            CommandManager::get_instance()->add_command(option_.name_, cmd);
        }

        if (option_.db_ != 0)
        {
            auto cmd = std::make_shared<DefaultCmd>();
            cmd->set_args("select", { StringValue::from_string(std::to_string(option_.db_))});
            CommandManager::get_instance()->add_command(option_.name_, cmd);
        }

        conn_ = conn;
        set_mask(RedisState::connected);
    }

    void RedisClient::Impl::close()
    {
        if (conn_)
        {
            conn_->close();
        }
    }

    void RedisClient::Impl::on_read(net::Connection *conn)
    {
        if ((!last_cmd_ && !subcribe_cmd_) || !is_connected())
        {
            return;
        }

        auto buff = conn->get_input_buff();
        if (conn->get_input_linked_buffer()->get_size() > 1) {
            conn->get_input_linked_buffer()->foreach([this, buff](buffer::Buffer *buf) -> bool {
                if (buf != buff) {
                    buff->append_buffer(*buf);
                }
                return true;
            });
        }

        auto cmd = last_cmd_ ? last_cmd_ : subcribe_cmd_;
        int ret = cmd->unpack((const unsigned char *)buff->peek(), (const unsigned char *)buff->peek_end());
        if (ret < 0)
        {
            last_error_ = ErrorValue::to_error((const unsigned char *)buff->peek(), (const unsigned char *)buff->peek_end());
            if (!last_error_)
            {
                last_error_ = ErrorValue::from_string("unknown error");
            }
        } else {
            if (cmd->get_result()->get_type() == resp_error) {
                last_error_ = cmd->get_result();
                cmd->set_result(nullptr);
            }
        }

        if (last_cmd_ == subcribe_cmd_) {
            if (subcribe_cmd_->get_result()) {
                subcribe_cmd_->set_is_subcribe(true);
            }
        }

        if (!last_cmd_ && subcribe_cmd_) {
            if (!subcribe_cmd_->is_subcribe()) {
                last_error_ = ErrorValue::from_string("subcribe failed");
            } else {
                subcribe_cmd_->exec_callback();
            }
        }

        resume();
        last_cmd_ = nullptr;
    }

    void RedisClient::Impl::on_write(net::Connection *conn)
    {
        if (last_cmd_)
        {
            return;
        }

        last_cmd_ = CommandManager::get_instance()->get_command(option_.name_);
        if (last_cmd_)
        {
            const auto& cmdStr = last_cmd_->pack();
            conn->get_output_buff()->write_string(cmdStr);
        }
    }

    SimpleTask<std::shared_ptr<RedisValue>> _do_execute_command(std::shared_ptr<Command> cmd)
    {
        co_await std::suspend_always{};
        RedisRegistry::get_instance()->get_event_loop()->loop();
        co_return cmd->get_result();
    }

    SimpleTask<std::shared_ptr<RedisValue>> do_execute_command(std::shared_ptr<Command> cmd)
    {
        auto co = _do_execute_command(cmd);
        co_return _do_execute_command(cmd).execute();
    }

    std::shared_ptr<RedisValue> RedisClient::Impl::execute_command(std::shared_ptr<Command> cmd)
    {
        if (!is_connecting()) {
            if (!client_) {
                last_error_ = ErrorValue::from_string("unexpected error");
                return nullptr;
            }

            int ret = client_->connect();
            if (ret < 0) {
                last_error_ = ErrorValue::from_string("connect failed");
                return nullptr;
            }
        }

        if (!is_connected()) {
            last_error_ = ErrorValue::from_string("not connected");
            return nullptr;
        }

        if (multi_cmd_) {
            multi_cmd_->add_command(cmd);
            return nullptr;
        }

        last_error_ = nullptr;

        CommandManager::get_instance()->add_command(option_.name_, cmd);
        auto co = do_execute_command(cmd);
        return co.execute();
    }
}