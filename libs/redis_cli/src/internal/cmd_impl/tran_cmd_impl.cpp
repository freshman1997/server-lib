#include "cmd/default_cmd.h"
#include "cmd/multi_cmd.h"
#include "internal/command_manager.h"
#include "redis_client.h"
#include "internal/redis_impl.h"
#include "value/error_value.h"
#include "value/string_value.h"

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
        CommandManager::get_instance()->add_command(impl_->option_.name_, cmd);
        
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

        return impl_->execute_command(multi_cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::discard()
    {
        if (!impl_->multi_cmd_)
        {
            impl_->last_error_ = ErrorValue::from_string("ERR: DISCARD without MULTI");
            return nullptr;
        }
        
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("discard", {});
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::watch(const std::vector<std::string> &keys)
    {
        if (keys.empty())
        {
            impl_->last_error_ = ErrorValue::from_string("ERR: WATCH without keys");
            return nullptr;
        }
        
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("watch", {});
        for (auto &key : keys)
        {
            cmd->add_arg(StringValue::from_string(key));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::unwatch()
    {
        if (!impl_->multi_cmd_)
        {
            impl_->last_error_ = ErrorValue::from_string("ERR: UNWATCH without MULTI");
            return nullptr;
        }
        
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("unwatch", {});
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::multi_exec(const std::vector<std::string> &commands)
    {
        if (commands.empty())
        {
            impl_->last_error_ = ErrorValue::from_string("ERR: empty commands");
            return nullptr;
        }
        
        if (multi())
        {
            impl_->last_error_ = ErrorValue::from_string("ERR: already in multi");
            return nullptr;
        }
        
        auto cmd = std::make_shared<DefaultCmd>();

        for (auto &command : commands)
        {
            cmd->add_arg(StringValue::from_string(command));
        }

        cmd->set_args("exec", {});
        
        return impl_->execute_command(cmd);
    }
}