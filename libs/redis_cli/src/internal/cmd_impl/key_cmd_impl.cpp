#include "../redis_impl.h"
#include "internal/cmd_builder.h"
#include "redis_client.h"
#include "value/error_value.h"

namespace yuan::redis
{
    namespace
    {
        bool require_non_empty_keys(RedisClient &client, const std::vector<std::string> &keys, const char *cmd_name)
        {
            if (!keys.empty()) {
                return true;
            }

            client.set_last_error(ErrorValue::from_string(std::string("ERR: ") + cmd_name + " requires at least one key"));
            return false;
        }
    }

    std::shared_ptr<RedisValue> RedisClient::del(const std::vector<std::string> &keys)
    {
        if (!require_non_empty_keys(*this, keys, "DEL")) {
            return nullptr;
        }

        auto cmd = make_cmd("del");
        append_args(cmd, keys);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::unlink(const std::vector<std::string> &keys)
    {
        if (!require_non_empty_keys(*this, keys, "UNLINK")) {
            return nullptr;
        }

        auto cmd = make_cmd("unlink");
        append_args(cmd, keys);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::exists(const std::vector<std::string> &keys)
    {
        if (!require_non_empty_keys(*this, keys, "EXISTS")) {
            return nullptr;
        }

        auto cmd = make_cmd("exists");
        append_args(cmd, keys);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::expire(std::string key, int seconds)
    {
        return impl_->execute_command(make_cmd("expire", std::move(key), seconds));
    }

    std::shared_ptr<RedisValue> RedisClient::expireat(std::string key, int64_t unix_time_seconds)
    {
        return impl_->execute_command(make_cmd("expireat", std::move(key), unix_time_seconds));
    }

    std::shared_ptr<RedisValue> RedisClient::pexpire(std::string key, int64_t milliseconds)
    {
        return impl_->execute_command(make_cmd("pexpire", std::move(key), milliseconds));
    }

    std::shared_ptr<RedisValue> RedisClient::pexpireat(std::string key, int64_t unix_time_milliseconds)
    {
        return impl_->execute_command(make_cmd("pexpireat", std::move(key), unix_time_milliseconds));
    }

    std::shared_ptr<RedisValue> RedisClient::ttl(std::string key)
    {
        return impl_->execute_command(make_cmd("ttl", std::move(key)));
    }

    std::shared_ptr<RedisValue> RedisClient::pttl(std::string key)
    {
        return impl_->execute_command(make_cmd("pttl", std::move(key)));
    }

    std::shared_ptr<RedisValue> RedisClient::expiretime(std::string key)
    {
        return impl_->execute_command(make_cmd("expiretime", std::move(key)));
    }

    std::shared_ptr<RedisValue> RedisClient::pexpiretime(std::string key)
    {
        return impl_->execute_command(make_cmd("pexpiretime", std::move(key)));
    }

    std::shared_ptr<RedisValue> RedisClient::persist(std::string key)
    {
        return impl_->execute_command(make_cmd("persist", std::move(key)));
    }

    std::shared_ptr<RedisValue> RedisClient::touch(const std::vector<std::string> &keys)
    {
        if (!require_non_empty_keys(*this, keys, "TOUCH")) {
            return nullptr;
        }

        auto cmd = make_cmd("touch");
        append_args(cmd, keys);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::randomkey()
    {
        return impl_->execute_command(make_cmd("randomkey"));
    }

    std::shared_ptr<RedisValue> RedisClient::move(std::string key, int db)
    {
        return impl_->execute_command(make_cmd("move", std::move(key), db));
    }

    std::shared_ptr<RedisValue> RedisClient::rename(std::string key, std::string new_key)
    {
        return impl_->execute_command(make_cmd("rename", std::move(key), std::move(new_key)));
    }

    std::shared_ptr<RedisValue> RedisClient::renamenx(std::string key, std::string new_key)
    {
        return impl_->execute_command(make_cmd("renamenx", std::move(key), std::move(new_key)));
    }

    std::shared_ptr<RedisValue> RedisClient::key_type(std::string key)
    {
        return impl_->execute_command(make_cmd("type", std::move(key)));
    }

    std::shared_ptr<RedisValue> RedisClient::keys(std::string pattern)
    {
        return impl_->execute_command(make_cmd("keys", std::move(pattern)));
    }

    std::shared_ptr<RedisValue> RedisClient::scan(int64_t cursor, const std::string &match_pattern, int64_t count)
    {
        auto cmd = make_cmd("scan", cursor);
        if (!match_pattern.empty()) {
            append_arg(cmd, "MATCH");
            append_arg(cmd, match_pattern);
        }
        if (count > 0) {
            append_arg(cmd, "COUNT");
            append_arg(cmd, count);
        }
        return impl_->execute_command(cmd);
    }
}
