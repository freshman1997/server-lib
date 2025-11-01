#include "redis_client.h"
#include "cmd/default_cmd.h"
#include "value/string_value.h"
#include "../redis_impl.h"

namespace yuan::redis 
{
    std::shared_ptr<RedisValue> RedisClient::get(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("get", {std::make_shared<StringValue>(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::set(std::string key, std::string value)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("set", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(value)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::set(std::string key, std::string value, int expire)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("set", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(value),
                             std::make_shared<StringValue>("EX"), std::make_shared<StringValue>(std::to_string(expire))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::set(std::string key, std::string value, int expire, int nx)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("set", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(value),
                             std::make_shared<StringValue>("EX"), std::make_shared<StringValue>(std::to_string(expire)),
                             std::make_shared<StringValue>("NX")});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::set(std::string key, std::string value, int expire, int nx, int xx)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("set", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(value),
                             std::make_shared<StringValue>("EX"), std::make_shared<StringValue>(std::to_string(expire)),
                             std::make_shared<StringValue>("NX"), std::make_shared<StringValue>("XX")});
        return impl_->execute_command(cmd);
    }
}