#include "redis_impl.h"
#include "event/event_loop.h"
#include "internal/command_manager.h"
#include "internal/redis_registry.h"
#include "value/error_value.h"

namespace yuan::redis 
{
    void RedisClient::Impl::on_read(net::Connection *conn)
    {
        if (!last_cmd_)
        {
            return;
        }

        auto buff = conn->get_input_buff();
        int ret = last_cmd_->unpack((const unsigned char *)buff->peek(), (const unsigned char *)buff->peek_end());
        if (ret < 0)
        {
            last_error_ = ErrorValue::to_error((const unsigned char *)buff->peek(), (const unsigned char *)buff->peek_end());
            if (!last_error_)
            {
                last_error_ = ErrorValue::from_string("unknown error");
            }
        }
        RedisRegistry::get_instance()->get_event_loop()->set_use_coroutine(true);
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
}