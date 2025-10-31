#include "cmd/default_cmd.h"
#include "redis_client.h"
#include "value/string_value.h"
#include <memory>

namespace yuan::redis 
{
    std::shared_ptr<RedisValue> RedisClient::eval(std::string script, const std::vector<std::string> &keys, const std::vector<std::string> &args)
    {
        if (script.empty())
        {
            return nullptr;
        }

        if (keys.empty() && args.empty())
        {
            return nullptr;
        }

        if (keys.size() != args.size())
        {
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
        
        return execute_command(cmd).get_result();
    }

    std::shared_ptr<RedisValue> RedisClient::evalsha(std::string sha1, const std::vector<std::string> &keys, const std::vector<std::string> &args)
    {
        if (sha1.empty())
        {
            return nullptr;
        }
        
        if (keys.empty() && args.empty())
        {
            return nullptr;
        }

        if (keys.size() != args.size())
        {
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
        
        return execute_command(cmd).get_result();
    }

    std::shared_ptr<RedisValue> RedisClient::script_load(std::string script)
    {
        if (script.empty())
        {
            return nullptr;
        }
        
        auto cmd = std::make_shared<DefaultCmd>();
        std::vector<std::shared_ptr<RedisValue>> scriptArgs;
        scriptArgs.push_back(StringValue::from_string(script));
        cmd->set_args("SCRIPT", scriptArgs);
        
        return execute_command(cmd).get_result();
    }

    std::shared_ptr<RedisValue> RedisClient::script_exists(const std::vector<std::string> &sha1s)
    {
        return nullptr;
    }

    std::shared_ptr<RedisValue> RedisClient::script_flush()
    {
        return nullptr;
    }

    std::shared_ptr<RedisValue> RedisClient::script_kill()
    {
        return nullptr;
    }

    std::shared_ptr<RedisValue> RedisClient::script_flush(const std::vector<std::string> &keys)
    {
        return nullptr;
    }
}