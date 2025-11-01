#include "cmd/default_cmd.h"
#include "redis_client.h"
#include "value/error_value.h"
#include "value/string_value.h"
#include "../redis_impl.h"

namespace yuan::redis 
{
    std::shared_ptr<RedisValue> RedisClient::eval(std::string script, const std::vector<std::string> &keys, const std::vector<std::string> &args)
    {
        if (script.empty())
        {
            impl_->last_error_ = ErrorValue::from_string("script is empty");
            return nullptr;
        }

        if (keys.empty() && args.empty())
        {
            impl_->last_error_ = ErrorValue::from_string("keys and args is empty");
            return nullptr;
        }

        if (keys.size() != args.size())
        {
            impl_->last_error_ = ErrorValue::from_string("keys and args size not match");
            return nullptr;
        }

        auto cmd = std::make_shared<DefaultCmd>();
        std::vector<std::shared_ptr<RedisValue>> scriptArgs;
        scriptArgs.push_back(StringValue::from_string(script));
        scriptArgs.push_back(StringValue::from_string(std::to_string(keys.size())));

        for (auto &key : keys)
        {
            scriptArgs.push_back(StringValue::from_string(key));
        }
        
        for (auto &arg : args)
        {
            scriptArgs.push_back(StringValue::from_string(arg));
        }

        cmd->set_args("EVAL", scriptArgs);
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::evalsha(std::string sha1, const std::vector<std::string> &keys, const std::vector<std::string> &args)
    {
        if (sha1.empty())
        {
            impl_->last_error_ = ErrorValue::from_string("sha1 is empty");
            return nullptr;
        }
        
        if (keys.empty() && args.empty())
        {
            impl_->last_error_ = ErrorValue::from_string("keys and args is empty");
            return nullptr;
        }

        if (keys.size() != args.size())
        {
            impl_->last_error_ = ErrorValue::from_string("keys and args size not match");
            return nullptr;
        }

        auto cmd = std::make_shared<DefaultCmd>();
        std::vector<std::shared_ptr<RedisValue>> scriptArgs;
        scriptArgs.push_back(StringValue::from_string(sha1));
        scriptArgs.push_back(StringValue::from_string(std::to_string(keys.size())));
        
        for (auto &key : keys)
        {
            scriptArgs.push_back(StringValue::from_string(key));
        }
        
        for (auto &arg : args)
        {
            scriptArgs.push_back(StringValue::from_string(arg));
        }
        
        cmd->set_args("EVALSHA", scriptArgs);
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::script_load(std::string script)
    {
        if (script.empty())
        {
            impl_->last_error_ = ErrorValue::from_string("script is empty");
            return nullptr;
        }
        
        auto cmd = std::make_shared<DefaultCmd>();
        std::vector<std::shared_ptr<RedisValue>> scriptArgs;
        scriptArgs.push_back(StringValue::from_string("load"));
        scriptArgs.push_back(StringValue::from_string(script));
        cmd->set_args("script", scriptArgs);
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::script_exists(const std::string &sha1s)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        std::vector<std::shared_ptr<RedisValue>> scriptArgs;
        scriptArgs.push_back(StringValue::from_string("exists"));
        scriptArgs.push_back(StringValue::from_string(sha1s));
        cmd->set_args("script", scriptArgs);
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::script_exists(const std::vector<std::string> &sha1s)
    {
        if (sha1s.empty())
        {
            impl_->last_error_ = ErrorValue::from_string("sha1s is empty");
            return nullptr;
        }
        
        auto cmd = std::make_shared<DefaultCmd>();
        std::vector<std::shared_ptr<RedisValue>> scriptArgs;
        scriptArgs.push_back(StringValue::from_string("exists"));
        for (auto &sha1 : sha1s)
        {
            scriptArgs.push_back(StringValue::from_string(sha1));
        }
        cmd->set_args("script", scriptArgs);
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::script_flush()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        std::vector<std::shared_ptr<RedisValue>> scriptArgs;
        scriptArgs.push_back(StringValue::from_string("flush"));
        cmd->set_args("script", scriptArgs);
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::script_kill()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        std::vector<std::shared_ptr<RedisValue>> scriptArgs;
        scriptArgs.push_back(StringValue::from_string("kill"));
        cmd->set_args("script", scriptArgs);
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::script_flush(const std::vector<std::string> &keys)
    {
        if (keys.empty())
        {
            impl_->last_error_ = ErrorValue::from_string("keys is empty");
            return nullptr;
        }
        
        auto cmd = std::make_shared<DefaultCmd>();
        std::vector<std::shared_ptr<RedisValue>> scriptArgs;
        scriptArgs.push_back(StringValue::from_string("flush"));
        for (auto &key : keys)
        {
            scriptArgs.push_back(StringValue::from_string(key));
        }
        cmd->set_args("script", scriptArgs);
        
        return impl_->execute_command(cmd);
    }
}