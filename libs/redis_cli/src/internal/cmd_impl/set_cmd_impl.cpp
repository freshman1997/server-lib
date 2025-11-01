#include "cmd/default_cmd.h"
#include "redis_client.h"
#include "value/string_value.h"
#include "../redis_impl.h"

namespace yuan::redis 
{
    std::shared_ptr<RedisValue> RedisClient::sadd(std::string key, const std::vector<std::string> &members)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("sadd", {std::make_shared<StringValue>(key)});
        for (auto &member : members)
        {
            cmd->add_arg(std::make_shared<StringValue>(member));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::srem(std::string key, const std::vector<std::string> &members)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("srem", {std::make_shared<StringValue>(key)});
        for (auto &member : members)
        {
            cmd->add_arg(std::make_shared<StringValue>(member));
        }
        return impl_->execute_command(cmd);
    }
    
    std::shared_ptr<RedisValue> RedisClient::smembers(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("smembers", {std::make_shared<StringValue>(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sismember(std::string key, std::string member)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("sismember", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(member)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::scard(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("scard", {std::make_shared<StringValue>(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::srandmember(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("srandmember", {std::make_shared<StringValue>(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::srandmember(std::string key, int count)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("srandmember", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(count))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::spop(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("spop", {std::make_shared<StringValue>(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::spop(std::string key, int count)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("spop", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(count))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::smove(std::string source, std::string destination, std::string member)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("smove", {std::make_shared<StringValue>(source), std::make_shared<StringValue>(destination), std::make_shared<StringValue>(member)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sscan(std::string key, int64_t cursor, const std::string &match_pattern /*= ""*/, int64_t count /*= 10*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("sscan", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(cursor))});
        if (!match_pattern.empty())
        {
            cmd->add_arg(std::make_shared<StringValue>("MATCH"));
            cmd->add_arg(std::make_shared<StringValue>(match_pattern));
        }
        if (count > 0)
        {
            cmd->add_arg(std::make_shared<StringValue>("COUNT"));
            cmd->add_arg(std::make_shared<StringValue>(std::to_string(count)));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sscan(std::string key, int64_t cursor, const std::vector<std::string> &match_patterns, int64_t count /*= 10*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("sscan", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(cursor))});
        if (!match_patterns.empty())
        {
            cmd->add_arg(std::make_shared<StringValue>("MATCH"));
            for (auto &pattern : match_patterns)
            {
                cmd->add_arg(std::make_shared<StringValue>(pattern));
            }
        }
        if (count > 0)
        {
            cmd->add_arg(std::make_shared<StringValue>("COUNT"));
            cmd->add_arg(std::make_shared<StringValue>(std::to_string(count)));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sdiff(const std::vector<std::string> &keys)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("sdiff", {});
        for (auto &member : keys)
        {
            cmd->add_arg(std::make_shared<StringValue>(member));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sdiffstore(std::string destination, const std::vector<std::string> &keys)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("sdiffstore", {std::make_shared<StringValue>(destination)});
        for (auto &member : keys)
        {
            cmd->add_arg(std::make_shared<StringValue>(member));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sinter(const std::vector<std::string> &keys)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("sinter", {});
        for (auto &member : keys)
        {
            cmd->add_arg(std::make_shared<StringValue>(member));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sinterstore(std::string destination, const std::vector<std::string> &keys)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("sinterstore", {std::make_shared<StringValue>(destination)});
        for (auto &member : keys)
        {
            cmd->add_arg(std::make_shared<StringValue>(member));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sunion(const std::vector<std::string> &keys)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("sunion", {});
        for (auto &member : keys)
        {
            cmd->add_arg(std::make_shared<StringValue>(member));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::sunionstore(std::string destination, const std::vector<std::string> &keys)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("sunionstore", {std::make_shared<StringValue>(destination)});
        for (auto &member : keys)
        {
            cmd->add_arg(std::make_shared<StringValue>(member));
        }
        return impl_->execute_command(cmd);
    }
}