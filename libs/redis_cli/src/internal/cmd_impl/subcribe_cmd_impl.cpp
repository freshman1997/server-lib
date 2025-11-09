#include "redis_client.h"
#include "../redis_impl.h"
#include "cmd/default_cmd.h"
#include "cmd/subcribe_cmd.h"
#include "value/string_value.h"

namespace yuan::redis
{
    std::shared_ptr<RedisValue> RedisClient::publish(std::string channel, std::string message)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("publish", {
            std::make_shared<StringValue>(channel),
            std::make_shared<StringValue>(message)
        });
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::subscribe(const std::vector<std::string> &channels, std::function<void(const std::unordered_map<std::string, std::shared_ptr<RedisValue>> &)> callback)
    {
        auto cmd = std::make_shared<SubcribeCmd>();
        cmd->set_args("subscribe", {});
        cmd->set_callback(std::move(callback));
        cmd->set_channels(channels);

        for (auto &channel : channels)
        {
            cmd->add_arg(std::make_shared<StringValue>(channel));
        }
        
        impl_->subcribe_cmd = cmd;
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::psubscribe(const std::vector<std::string> &patterns)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("psubscribe", {});
        
        for (auto &pattern : patterns)
        {
            cmd->add_arg(std::make_shared<StringValue>(pattern));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::unsubscribe(const std::vector<std::string> &channels)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("unsubscribe", {});
        
        for (auto &channel : channels)
        {
            cmd->add_arg(std::make_shared<StringValue>(channel));
        }

        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::punsubscribe(const std::vector<std::string> &patterns)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("punsubscribe", {});
        
        for (auto &pattern : patterns)
        {
            cmd->add_arg(std::make_shared<StringValue>(pattern));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::psubscribe(const std::vector<std::string> &patterns, const std::vector<std::string> &channels)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("psubscribe", {});
        
        for (auto &pattern : patterns)
        {
            cmd->add_arg(std::make_shared<StringValue>(pattern));
        }
        
        for (auto &channel : channels)
        {
            cmd->add_arg(std::make_shared<StringValue>(channel));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::unsubscribe(const std::vector<std::string> &channels, const std::vector<std::string> &patterns)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("unsubscribe", {});
        
        for (auto &channel : channels)
        {
            cmd->add_arg(std::make_shared<StringValue>(channel));
        }
        
        for (auto &pattern : patterns)
        {
            cmd->add_arg(std::make_shared<StringValue>(pattern));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::psubscribe(const std::vector<std::string> &patterns, const std::vector<std::string> &channels, const std::vector<std::string> &unsubscribe_patterns)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("psubscribe", {});
        
        for (auto &pattern : patterns)
        {
            cmd->add_arg(std::make_shared<StringValue>(pattern));
        }
        
        for (auto &channel : channels)
        {
            cmd->add_arg(std::make_shared<StringValue>(channel));
        }
        
        for (auto &pattern : unsubscribe_patterns)
        {
            cmd->add_arg(std::make_shared<StringValue>(pattern));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::unsubscribe(const std::vector<std::string> &channels, const std::vector<std::string> &patterns, const std::vector<std::string> &unsubscribe_channels)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("unsubscribe", {});
        
        for (auto &channel : channels)
        {
            cmd->add_arg(std::make_shared<StringValue>(channel));
        }
        
        for (auto &pattern : patterns)
        {
            cmd->add_arg(std::make_shared<StringValue>(pattern));
        }
        
        for (auto &channel : unsubscribe_channels)
        {
            cmd->add_arg(std::make_shared<StringValue>(channel));
        }
        
        return impl_->execute_command(cmd);
    }
}
