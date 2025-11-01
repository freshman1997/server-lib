#include "cmd/default_cmd.h"
#include "redis_client.h"
#include "value/string_value.h"
#include "../redis_impl.h"
#include "../utils.h"

namespace yuan::redis 
{
    std::shared_ptr<RedisValue> RedisClient::hget(std::string key, std::string field)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hget", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(field)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hset(std::string key, std::string field, std::string value)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hset", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(field), std::make_shared<StringValue>(value)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hset(std::string key, const std::unordered_map<std::string, std::string> &field_values)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hset", {std::make_shared<StringValue>(key)});
        for (auto &[field, value] : field_values)
        {
            cmd->add_arg(std::make_shared<StringValue>(field));
            cmd->add_arg(std::make_shared<StringValue>(value));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hmset(std::string key, const std::unordered_map<std::string, std::string> &field_values)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hmset", {std::make_shared<StringValue>(key)});
        for (auto &[field, value] : field_values)
        {
            cmd->add_arg(std::make_shared<StringValue>(field));
            cmd->add_arg(std::make_shared<StringValue>(value));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hmget(std::string key, const std::vector<std::string> &fields)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hmget", {std::make_shared<StringValue>(key)});
        for (auto &field : fields)
        {
            cmd->add_arg(std::make_shared<StringValue>(field));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hgetall(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hgetall", {std::make_shared<StringValue>(key)});
        cmd->set_unpack_to_map(true);
        return impl_->execute_command(cmd);
    }
    std::shared_ptr<RedisValue> RedisClient::hdel(std::string key, const std::vector<std::string> &fields)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hdel", {std::make_shared<StringValue>(key)});
        for (auto &field : fields)
        {
            cmd->add_arg(std::make_shared<StringValue>(field));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hlen(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hlen", {std::make_shared<StringValue>(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hkeys(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hkeys", {std::make_shared<StringValue>(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hvals(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hvals", {std::make_shared<StringValue>(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hincrby(std::string key, std::string field, int64_t increment)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hincrby", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(field), std::make_shared<StringValue>(std::to_string(increment))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hincrbyfloat(std::string key, std::string field, double increment)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hincrbyfloat", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(field), std::make_shared<StringValue>(serializeDouble(increment))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hexists(std::string key, std::string field)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("hexists", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(field)});
        return impl_->execute_command(cmd);
    }
}