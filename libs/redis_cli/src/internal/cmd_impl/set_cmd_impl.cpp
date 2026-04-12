#include "internal/cmd_builder.h"
#include "redis_client.h"
#include "value/error_value.h"
#include "../redis_impl.h"

namespace yuan::redis 
{
    std::shared_ptr<RedisValue> RedisClient::sadd(std::string key, const std::vector<std::string> &members)
    {
        auto cmd = make_cmd("sadd", key);
        append_args(cmd, members);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::srem(std::string key, const std::vector<std::string> &members)
    {
        auto cmd = make_cmd("srem", key);
        append_args(cmd, members);
        return impl_->execute_command(cmd);
    }
    
    std::shared_ptr<RedisValue> RedisClient::smembers(std::string key)
    {
        return impl_->execute_command(make_cmd("smembers", key));
    }

    std::shared_ptr<RedisValue> RedisClient::sismember(std::string key, std::string member)
    {
        return impl_->execute_command(make_cmd("sismember", key, member));
    }

    std::shared_ptr<RedisValue> RedisClient::scard(std::string key)
    {
        return impl_->execute_command(make_cmd("scard", key));
    }

    std::shared_ptr<RedisValue> RedisClient::srandmember(std::string key)
    {
        return impl_->execute_command(make_cmd("srandmember", key));
    }

    std::shared_ptr<RedisValue> RedisClient::srandmember(std::string key, int count)
    {
        return impl_->execute_command(make_cmd("srandmember", key, count));
    }

    std::shared_ptr<RedisValue> RedisClient::spop(std::string key)
    {
        return impl_->execute_command(make_cmd("spop", key));
    }

    std::shared_ptr<RedisValue> RedisClient::spop(std::string key, int count)
    {
        return impl_->execute_command(make_cmd("spop", key, count));
    }

    std::shared_ptr<RedisValue> RedisClient::smove(std::string source, std::string destination, std::string member)
    {
        return impl_->execute_command(make_cmd("smove", source, destination, member));
    }

    std::shared_ptr<RedisValue> RedisClient::sscan(std::string key, int64_t cursor, const std::string &match_pattern /*= ""*/, int64_t count /*= 10*/)
    {
        auto cmd = make_cmd("sscan", key, cursor);
        if (!match_pattern.empty())
        {
            append_arg(cmd, "MATCH");
            append_arg(cmd, match_pattern);
        }
        if (count > 0)
        {
            append_arg(cmd, "COUNT");
            append_arg(cmd, count);
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sscan(std::string key, int64_t cursor, const std::vector<std::string> &match_patterns, int64_t count /*= 10*/)
    {
        if (match_patterns.size() > 1)
        {
            set_last_error(ErrorValue::from_string("ERR: SSCAN supports at most one MATCH pattern"));
            return nullptr;
        }

        auto cmd = make_cmd("sscan", key, cursor);
        if (!match_patterns.empty())
        {
            append_arg(cmd, "MATCH");
            append_arg(cmd, match_patterns.front());
        }
        if (count > 0)
        {
            append_arg(cmd, "COUNT");
            append_arg(cmd, count);
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sdiff(const std::vector<std::string> &keys)
    {
        auto cmd = make_cmd("sdiff");
        append_args(cmd, keys);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sdiffstore(std::string destination, const std::vector<std::string> &keys)
    {
        auto cmd = make_cmd("sdiffstore", destination);
        append_args(cmd, keys);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sinter(const std::vector<std::string> &keys)
    {
        auto cmd = make_cmd("sinter");
        append_args(cmd, keys);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sinterstore(std::string destination, const std::vector<std::string> &keys)
    {
        auto cmd = make_cmd("sinterstore", destination);
        append_args(cmd, keys);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sunion(const std::vector<std::string> &keys)
    {
        auto cmd = make_cmd("sunion");
        append_args(cmd, keys);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sunionstore(std::string destination, const std::vector<std::string> &keys)
    {
        auto cmd = make_cmd("sunionstore", destination);
        append_args(cmd, keys);
        return impl_->execute_command(cmd);
    }
}
