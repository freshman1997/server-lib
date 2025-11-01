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
            CommandManager::get_instance()->add_command(cmd);
        }

        if (option_.db_ != 0)
        {
            auto cmd = std::make_shared<DefaultCmd>();
            cmd->set_args("select", { StringValue::from_string(std::to_string(option_.db_))});
            CommandManager::get_instance()->add_command(cmd);
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
        if (!last_cmd_ || !is_connected())
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

        int ret = last_cmd_->unpack((const unsigned char *)buff->peek(), (const unsigned char *)buff->peek_end());
        if (ret < 0)
        {
            last_error_ = ErrorValue::to_error((const unsigned char *)buff->peek(), (const unsigned char *)buff->peek_end());
            if (!last_error_)
            {
                last_error_ = ErrorValue::from_string("unknown error");
            }
        } else {
            if (last_cmd_->get_result()->get_type() == resp_error) {
                last_error_ = last_cmd_->get_result();
                last_cmd_->set_result(nullptr);
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

        last_cmd_ = CommandManager::get_instance()->get_command();
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
        CommandManager::get_instance()->add_command(cmd);
        auto co = _do_execute_command(cmd);
        co_return co.execute();
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

        auto co = do_execute_command(cmd);
        return co.execute();
    }
}