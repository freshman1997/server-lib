#include "redis_impl.h"
#include "base/time.h"
#include "buffer/buffer.h"
#include "buffer/linked_buffer.h"
#include "coroutine.h"
#include "def.h"
#include "event/event_loop.h"
#include "internal/command_manager.h"
#include "internal/redis_registry.h"
#include "timer/timer.h"
#include "timer/timer_manager.h"
#include "value/array_value.h"
#include "value/error_value.h"
#include "value/status_value.h"
#include "value/string_value.h"

#include <iostream>

namespace yuan::redis 
{
    static void resume()
    {
        RedisRegistry::get_instance()->get_event_loop()->set_use_coroutine(true);
    }

    void RedisClient::Impl::on_connected(net::Connection *conn)
    {
        set_mask(RedisState::connected);

        if (!option_.password_.empty())
        {
            if (const auto res = client_->auth(option_.password_); !res) {
                last_error_ = ErrorValue::from_string("auth failed");
                close();
                return;
            }
        }

        if (option_.db_ != 0)
        {
            if (const auto res = client_->select(option_.db_); !res) {
                last_error_ = ErrorValue::from_string("select db failed");
                close();
            }
        }
        
        resume();
    }

    void RedisClient::Impl::close()
    {
        if (!conn_ || is_executing()) {
            return;
        }

        set_mask(RedisState::closed, true);
        conn_->close();
        resume();
        conn_ = nullptr;
    }

    void RedisClient::Impl::on_read(net::Connection *conn)
    {
        if ((!last_cmd_ && !subcribe_cmd) || !is_connected())
        {
            return;
        }

        reader_.add_buffer(conn->get_input_linked_buffer()->to_vector(true));

        const auto cmd = last_cmd_ ? last_cmd_ : subcribe_cmd;
        if (const int ret = cmd->unpack(reader_); ret < 0)
        {
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

        if (last_cmd_ == subcribe_cmd) {
            if (subcribe_cmd->get_result()) {
                subcribe_cmd->set_is_subcribe(true);
            }
        }

        if (!last_cmd_ && subcribe_cmd) {
            if (!subcribe_cmd->is_subcribe()) {
                last_error_ = ErrorValue::from_string("subscribe failed");
            } else {
                subcribe_cmd->exec_callback();
            }
        }

        reader_.init();
        last_cmd_ = nullptr;

        if (is_timeout()) {
            close();
        }

        resume();
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

    SimpleTask<std::shared_ptr<RedisValue>> do_execute_command(std::shared_ptr<Command> cmd)
    {
        co_await std::suspend_always{};
        RedisRegistry::get_instance()->get_event_loop()->loop();
        co_return cmd->get_result();
    }

    std::shared_ptr<RedisValue> RedisClient::Impl::execute_command(std::shared_ptr<Command> cmd)
    {
        if (is_closed()) {
            return nullptr;
        }

        if (!is_connecting()) {
            if (!client_) {
                last_error_ = ErrorValue::from_string("unexpected error");
                return nullptr;
            }

            if (const int ret = client_->connect(); ret < 0) {
                last_error_ = ErrorValue::from_string(option_.name_ + " connect failed");
                return nullptr;
            }
        }

        if (!is_connected()) {
            last_error_ = ErrorValue::from_string("not connected");
            return nullptr;
        }

        check_timeout();

        if (is_timeout()) {
            close();
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
        conn_ = conn;

        if (option_.timeout_ms_ > 0) {
            last_check_time_ = base::time::now();
            RedisRegistry::get_instance()->get_timer_manager()->interval(0, 1000, this, -1);
        }
    }

    void RedisClient::Impl::on_timer(timer::Timer *timer)
    {
        check_timeout();

        if (!is_timeout() && is_connected()) {
            last_check_time_ = base::time::now();
            const auto res = client_->ping();
            if (last_error_) {
                last_error_ = ErrorValue::from_string("ping error");
                set_mask(RedisState::timeout);
            } else {
                if (res) {
                    if (res->get_type() == resp_array) {
                        if (const auto arr = res->as<ArrayValue>(); arr && arr->get_values().size() == 2) {
                            const auto pong = arr->get_values()[0];
                            if (const auto pongRes = arr->get_values()[1]; pong && pong->get_type() == resp_string && pongRes->get_type() == resp_string) {
                                const auto pongStr = pong->as<StringValue>();
                                if (const auto pongResStr = pongRes->as<StringValue>(); pongStr
                                        && pongStr->get_value() != "pong"
                                        && pongResStr
                                        && !pongResStr->get_value().empty()) {
                                    set_mask(RedisState::timeout);
                                }
                            }
                        }
                    } else if (res->get_type() == resp_status) {
                        if (const auto status = res->as<StatusValue>(); status && !status->is_ok()) {
                            set_mask(RedisState::timeout);
                        }
                    }
                } else {
                    std::cerr << "ping error: " << option_.name_ << "\n";
                }
            }
        }

        if (is_timeout()) {
            timer->cancel();
            close();
        }
    }

    void RedisClient::Impl::check_timeout()
    {
        if (option_.timeout_ms_ == 0) {
            return;
        }

        if (const auto now = base::time::now(); now - last_check_time_ > option_.timeout_ms_) {
            set_mask(RedisState::timeout);
            last_error_ = ErrorValue::from_string("connection time out");
            close();
        }
    }

}