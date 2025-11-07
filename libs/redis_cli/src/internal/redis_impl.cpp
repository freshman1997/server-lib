#include "redis_impl.h"
#include "base/time.h"
#include "buffer/buffer.h"
#include "coroutine.h"
#include "def.h"
#include "event/event_loop.h"
#include "internal/command_manager.h"
#include "internal/redis_registry.h"
#include "timer/timer.h"
#include "value/error_value.h"
#include "value/string_value.h"
#include "buffer/linked_buffer.h"
#include "timer/timer_manager.h"

#include <iostream>

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

        set_mask(RedisState::connected);
    }

    void RedisClient::Impl::close()
    {
        if (conn_)
        {
            set_mask(RedisState::closed);
            conn_->close();
        }
    }

    void RedisClient::Impl::on_read(net::Connection *conn)
    {
        if ((!last_cmd_ && !subcribe_cmd_) || !is_connected())
        {
            return;
        }

        reader_.add_buffer(conn->get_input_linked_buffer()->to_vector(true));

        auto cmd = last_cmd_ ? last_cmd_ : subcribe_cmd_;
        int ret = cmd->unpack(reader_);
        if (ret < 0)
        {
            if (ret == UnpackCode::need_more_bytes) {
                return;
            } else if (ret == UnpackCode::format_error) {
                last_error_ = ErrorValue::from_string("format error");
            } else {
                last_error_ = ErrorValue::from_string("unknown error");
            }
        } else {
            reader_.init();
            if (cmd->get_result() && cmd->get_result()->get_type() == resp_error) {
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
                last_error_ = ErrorValue::from_string(option_.name_ + " connect failed");
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

        set_mask(RedisState::exec_command);

        CommandManager::get_instance()->add_command(option_.name_, cmd);
        auto co = do_execute_command(cmd);
        auto res = co.execute();

        clear_mask(RedisState::exec_command);

        return res;
    }

    void RedisClient::Impl::on_do_connect(net::Connection *conn)
    {
        clear_mask();
        set_mask(RedisState::connecting);
        last_check_time_ = base::time::now();
        conn_ = conn;
        RedisRegistry::get_instance()->get_timer_manager()->interval(0, 1000, this, -1);
    }

    void RedisClient::Impl::on_timer(timer::Timer *timer)
    {
        auto now = base::time::now();
        if (now - last_check_time_ > 10000) {
            close();
            last_error_ = ErrorValue::from_string("connection time out");
            RedisRegistry::get_instance()->get_event_loop()->set_use_coroutine(true);
            timer->cancel();
        } else { 
            if (is_connected()) {
                last_check_time_ = now;
                auto ping = std::make_shared<DefaultCmd>();
                ping->set_args("ping", {});
                auto res = execute_command(ping);
                if (last_error_) {
                    std::cout << "ping error: " << last_error_->to_string() << "\n";
                } else {
                    std::cout << "ping success: " << res->to_string() << "\n";
                }
            }
        }
    }
}