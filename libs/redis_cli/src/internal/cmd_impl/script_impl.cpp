#include "internal/cmd_builder.h"
#include "redis_client.h"
#include "value/error_value.h"
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

        auto cmd = make_cmd("EVAL", script, keys.size());

        append_args(cmd, keys);
        append_args(cmd, args);
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::evalsha(std::string sha1, const std::vector<std::string> &keys, const std::vector<std::string> &args)
    {
        if (sha1.empty())
        {
            impl_->last_error_ = ErrorValue::from_string("sha1 is empty");
            return nullptr;
        }
        
        auto cmd = make_cmd("EVALSHA", sha1, keys.size());

        append_args(cmd, keys);
        append_args(cmd, args);
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::script_load(std::string script)
    {
        if (script.empty())
        {
            impl_->last_error_ = ErrorValue::from_string("script is empty");
            return nullptr;
        }
        
        return impl_->execute_command(make_cmd("script", "load", script));
    }

    std::shared_ptr<RedisValue> RedisClient::script_exists(const std::string &sha1s)
    {
        return impl_->execute_command(make_cmd("script", "exists", sha1s));
    }

    std::shared_ptr<RedisValue> RedisClient::script_exists(const std::vector<std::string> &sha1s)
    {
        if (sha1s.empty())
        {
            impl_->last_error_ = ErrorValue::from_string("sha1s is empty");
            return nullptr;
        }
        
        auto cmd = make_cmd("script", "exists");
        append_args(cmd, sha1s);
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::script_flush()
    {
        return impl_->execute_command(make_cmd("script", "flush"));
    }

    std::shared_ptr<RedisValue> RedisClient::script_kill()
    {
        return impl_->execute_command(make_cmd("script", "kill"));
    }

    std::shared_ptr<RedisValue> RedisClient::script_flush_with_args(const std::vector<std::string> &args)
    {
        auto cmd = make_cmd("script", "flush");
        append_args(cmd, args);
        
        return impl_->execute_command(cmd);
    }
}
