#include "redis_client.h"
#include "internal/cmd_builder.h"
#include "value/error_value.h"
#include "../redis_impl.h"

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

        bool require_non_empty_map(
            RedisClient &client,
            const std::unordered_map<std::string, std::string> &values,
            const char *cmd_name)
        {
            if (!values.empty()) {
                return true;
            }

            client.set_last_error(ErrorValue::from_string(std::string("ERR: ") + cmd_name + " requires at least one key/value pair"));
            return false;
        }
    }

    std::shared_ptr<RedisValue> RedisClient::get(std::string key)
    {
        return impl_->execute_command(make_cmd("get", key));
    }

    std::shared_ptr<RedisValue> RedisClient::mget(const std::vector<std::string> &keys)
    {
        if (!require_non_empty_keys(*this, keys, "MGET")) {
            return nullptr;
        }

        auto cmd = make_cmd("mget");
        append_args(cmd, keys);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::set(std::string key, std::string value)
    {
        return impl_->execute_command(make_cmd("set", key, value));
    }

    std::shared_ptr<RedisValue> RedisClient::mset(const std::unordered_map<std::string, std::string> &key_values)
    {
        if (!require_non_empty_map(*this, key_values, "MSET")) {
            return nullptr;
        }

        auto cmd = make_cmd("mset");
        for (const auto &[key, value] : key_values) {
            append_arg(cmd, key);
            append_arg(cmd, value);
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::msetnx(const std::unordered_map<std::string, std::string> &key_values)
    {
        if (!require_non_empty_map(*this, key_values, "MSETNX")) {
            return nullptr;
        }

        auto cmd = make_cmd("msetnx");
        for (const auto &[key, value] : key_values) {
            append_arg(cmd, key);
            append_arg(cmd, value);
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::set(std::string key, std::string value, int expire)
    {
        return impl_->execute_command(make_cmd("set", key, value, "EX", expire));
    }

    std::shared_ptr<RedisValue> RedisClient::set(std::string key, std::string value, int expire, int nx)
    {
        auto cmd = make_cmd("set", key, value, "EX", expire);
        if (nx) {
            append_arg(cmd, "NX");
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::set(std::string key, std::string value, int expire, int nx, int xx)
    {
        if (nx && xx) {
            impl_->last_error_.store(ErrorValue::from_string("ERR: NX and XX cannot be used together"));
            return nullptr;
        }

        auto cmd = make_cmd("set", key, value, "EX", expire);
        if (nx) {
            append_arg(cmd, "NX");
        }
        if (xx) {
            append_arg(cmd, "XX");
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::incr(std::string key)
    {
        return impl_->execute_command(make_cmd("incr", key));
    }

    std::shared_ptr<RedisValue> RedisClient::decr(std::string key)
    {
        return impl_->execute_command(make_cmd("decr", key));
    }

    std::shared_ptr<RedisValue> RedisClient::incrby(std::string key, int64_t increment)
    {
        return impl_->execute_command(make_cmd("incrby", key, increment));
    }

    std::shared_ptr<RedisValue> RedisClient::decrby(std::string key, int64_t decrement)
    {
        return impl_->execute_command(make_cmd("decrby", key, decrement));
    }
}
