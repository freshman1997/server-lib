#include "redis_impl.h"
#include "base/time.h"
#include "buffer/buffer.h"
#include "buffer/linked_buffer.h"
#include "coroutine.h"
#include "def.h"
#include "event/event_loop.h"
#include "internal/redis_registry.h"
#include "timer/timer.h"
#include "timer/timer_manager.h"
#include "value/array_value.h"
#include "value/error_value.h"
#include "value/status_value.h"
#include "value/string_value.h"
#include "timer/timer_util.hpp"

#include <cassert>
#include <iostream>

namespace yuan::redis 
{
    static void resume()
    {
        RedisRegistry::get_instance()->use_corutine();
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

        reader_.add_buffer(conn->get_input_buff());

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

        if (!last_cmd_ && subcribe_cmd) {
            if (!subcribe_cmd->is_subcribe()) {
                last_error_ = ErrorValue::from_string("subscribe failed");
            } else {
                subcribe_cmd->exec_callback();
            }
        }

        reader_.just_clear();
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

        if (pending_cmd_)
        {
            const auto& cmdStr = pending_cmd_->pack();
            conn->get_output_linked_buffer()->get_current_buffer()->write_string(cmdStr);
            last_cmd_ = pending_cmd_;
        }
    }

    SimpleTask<std::shared_ptr<RedisValue>> do_execute_command(std::shared_ptr<Command> cmd, RedisClient* client)
    {
        co_await std::suspend_always{};

        if (client->is_connected() && !client->is_closed()) {
            RedisRegistry::get_instance()->get_event_loop()->loop();
        }

        if (client->is_closed()) {
            client->set_last_error(ErrorValue::from_string("connection closed"));
        }

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

        if (pending_cmd_) {
            last_error_ = ErrorValue::from_string("executing");
            return nullptr;
        }

        last_error_ = nullptr;

        pending_cmd_ = cmd;
        last_cmd_ = nullptr;

        auto co = do_execute_command(cmd, client_);
        auto res = co.execute();

        pending_cmd_ = nullptr;
        last_cmd_ = nullptr;

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

    int RedisClient::Impl::fetch_next_message(int timeout)
    {
        if (is_closed()) {
            last_error_ = ErrorValue::from_string("connection closed");
            return -1;
        }

        if (!is_connected()) {
            last_error_ = ErrorValue::from_string("not connected");
            return -1;
        }

        return [timeout, this]() -> SimpleTask<int> {
            co_await std::suspend_always{};

            timer::Timer *timer = nullptr;
            bool is_timeout = false;
            if (timeout > 0) {
                timer = timer::TimerUtil::build_timeout_timer(RedisRegistry::get_instance()->get_timer_manager(), timeout, [this, &is_timeout](timer::Timer *t) {
                    last_error_ = ErrorValue::from_string("fetch message timeout");
                    t->cancel();
                    is_timeout = true;
                    RedisRegistry::get_instance()->use_corutine();
                });
            }

            RedisRegistry::get_instance()->run();

            if (!is_timeout && timer) {
                timer->cancel();
            }

            co_return is_timeout ? -1 : 0;

        }().execute();
    }
}