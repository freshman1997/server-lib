#include "cmd/default_cmd.h"
#include "redis_client.h"
#include "value/string_value.h"
#include "../redis_impl.h"

namespace yuan::redis 
{
    std::shared_ptr<RedisValue> RedisClient::lpush(std::string key, const std::vector<std::string> &values)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("lpush", {std::make_shared<StringValue>(key)});
        for (auto &value : values)
        {
            cmd->add_arg(std::make_shared<StringValue>(value));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::rpush(std::string key, const std::vector<std::string> &values)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("rpush", {std::make_shared<StringValue>(key)});
        for (auto &value : values)
        {
            cmd->add_arg(std::make_shared<StringValue>(value));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::lpop(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("lpop", {std::make_shared<StringValue>(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::rpop(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("rpop", {std::make_shared<StringValue>(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::lrange(std::string key, int64_t start, int64_t stop)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("lrange", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(start)), std::make_shared<StringValue>(std::to_string(stop))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::lindex(std::string key, int64_t index)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("lindex", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(index))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::llen(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("llen", {std::make_shared<StringValue>(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::lset(std::string key, int64_t index, std::string value)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("lset", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(index)), std::make_shared<StringValue>(value)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::lrem(std::string key, int64_t count, std::string value)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("lrem", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(count)), std::make_shared<StringValue>(value)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::ltrim(std::string key, int64_t start, int64_t stop)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("ltrim", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(start)), std::make_shared<StringValue>(std::to_string(stop))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::linsert(std::string key, std::string pivot, std::string value, bool before)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("linsert", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(pivot), std::make_shared<StringValue>(value), std::make_shared<StringValue>(before ? "BEFORE" : "AFTER")});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::linsert(std::string key, std::string pivot, const std::vector<std::string> &values, bool before)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("linsert", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(pivot)});
        for (auto &value : values)
        {
            cmd->add_arg(std::make_shared<StringValue>(value));
        }
        cmd->add_arg(std::make_shared<StringValue>(before ? "BEFORE" : "AFTER"));
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::rpoplpush(std::string source, std::string destination)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("rpoplpush", {std::make_shared<StringValue>(source), std::make_shared<StringValue>(destination)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::brpoplpush(std::string source, std::string destination, int timeout)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("brpoplpush", {std::make_shared<StringValue>(source), std::make_shared<StringValue>(destination), std::make_shared<StringValue>(std::to_string(timeout))});
        return impl_->execute_command(cmd);
    }
}