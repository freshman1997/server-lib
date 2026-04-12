#include "internal/cmd_builder.h"
#include "redis_client.h"
#include "../redis_impl.h"

namespace yuan::redis 
{
    std::shared_ptr<RedisValue> RedisClient::lpush(std::string key, const std::vector<std::string> &values)
    {
        auto cmd = make_cmd("lpush", key);
        append_args(cmd, values);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::rpush(std::string key, const std::vector<std::string> &values)
    {
        auto cmd = make_cmd("rpush", key);
        append_args(cmd, values);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::lpop(std::string key)
    {
        return impl_->execute_command(make_cmd("lpop", key));
    }

    std::shared_ptr<RedisValue> RedisClient::rpop(std::string key)
    {
        return impl_->execute_command(make_cmd("rpop", key));
    }

    std::shared_ptr<RedisValue> RedisClient::lrange(std::string key, int64_t start, int64_t stop)
    {
        return impl_->execute_command(make_cmd("lrange", key, start, stop));
    }

    std::shared_ptr<RedisValue> RedisClient::lindex(std::string key, int64_t index)
    {
        return impl_->execute_command(make_cmd("lindex", key, index));
    }

    std::shared_ptr<RedisValue> RedisClient::llen(std::string key)
    {
        return impl_->execute_command(make_cmd("llen", key));
    }

    std::shared_ptr<RedisValue> RedisClient::lset(std::string key, int64_t index, std::string value)
    {
        return impl_->execute_command(make_cmd("lset", key, index, value));
    }

    std::shared_ptr<RedisValue> RedisClient::lrem(std::string key, int64_t count, std::string value)
    {
        return impl_->execute_command(make_cmd("lrem", key, count, value));
    }

    std::shared_ptr<RedisValue> RedisClient::ltrim(std::string key, int64_t start, int64_t stop)
    {
        return impl_->execute_command(make_cmd("ltrim", key, start, stop));
    }

    std::shared_ptr<RedisValue> RedisClient::linsert(std::string key, std::string pivot, std::string value, bool before)
    {
        return impl_->execute_command(make_cmd("linsert", key, before ? "BEFORE" : "AFTER", pivot, value));
    }

    std::shared_ptr<RedisValue> RedisClient::linsert(std::string key, std::string pivot, const std::vector<std::string> &values, bool before)
    {
        std::shared_ptr<RedisValue> last_result;
        if (before) {
            for (const auto &value : values) {
                last_result = linsert(key, pivot, value, true);
                if (!last_result) {
                    return nullptr;
                }
            }
            return last_result;
        }

        for (auto it = values.rbegin(); it != values.rend(); ++it) {
            last_result = linsert(key, pivot, *it, false);
            if (!last_result) {
                return nullptr;
            }
        }
        return last_result;
    }

    std::shared_ptr<RedisValue> RedisClient::rpoplpush(std::string source, std::string destination)
    {
        return impl_->execute_command(make_cmd("rpoplpush", source, destination));
    }

    std::shared_ptr<RedisValue> RedisClient::brpoplpush(std::string source, std::string destination, int timeout)
    {
        return impl_->execute_command(make_cmd("brpoplpush", source, destination, timeout));
    }
}
