#include "cmd/default_cmd.h"
#include "cmd/multi_cmd.h"
#include "internal/command_manager.h"
#include "redis_client.h"
#include "internal/redis_impl.h"
#include "value/error_value.h"

namespace yuan::redis 
{
    bool RedisClient::multi()
    {
        if (impl_->multi_cmd_)
        {
            return true;
        }

        auto cmd = std::make_shared<MultiCmd>();
        cmd->set_args("multi", {});
        CommandManager::get_instance()->add_command(cmd);
        
        impl_->multi_cmd_ = cmd;

        return true;
    }

    std::shared_ptr<RedisValue> RedisClient::exec()
    {
        if (!impl_->multi_cmd_)
        {
            impl_->last_error_ = ErrorValue::from_string("ERR: EXEC without MULTI");
            return nullptr;
        }

        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("exec", {});

        impl_->multi_cmd_->add_command(cmd);
        auto multi_cmd = impl_->multi_cmd_;
        impl_->multi_cmd_ = nullptr;

        return execute_command(multi_cmd).get_result();
    }
}