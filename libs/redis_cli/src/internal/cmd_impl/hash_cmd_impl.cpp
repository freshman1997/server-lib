#include "internal/cmd_builder.h"
#include "redis_client.h"
#include "value/error_value.h"
#include "../redis_impl.h"
#include "../utils.h"

namespace yuan::redis 
{
    namespace
    {
        bool require_non_empty_fields(
            RedisClient &client,
            const std::vector<std::string> &fields,
            const char *cmd_name)
        {
            if (!fields.empty()) {
                return true;
            }

            client.set_last_error(ErrorValue::from_string(std::string("ERR: ") + cmd_name + " requires at least one field"));
            return false;
        }

        bool require_non_empty_field_values(
            RedisClient &client,
            const std::unordered_map<std::string, std::string> &field_values,
            const char *cmd_name)
        {
            if (!field_values.empty()) {
                return true;
            }

            client.set_last_error(ErrorValue::from_string(std::string("ERR: ") + cmd_name + " requires at least one field/value pair"));
            return false;
        }
    }

    std::shared_ptr<RedisValue> RedisClient::hget(std::string key, std::string field)
    {
        return impl_->execute_command(make_cmd("hget", key, field));
    }

    std::shared_ptr<RedisValue> RedisClient::hset(std::string key, std::string field, std::string value)
    {
        return impl_->execute_command(make_cmd("hset", key, field, value));
    }

    std::shared_ptr<RedisValue> RedisClient::hset(std::string key, const std::unordered_map<std::string, std::string> &field_values)
    {
        if (!require_non_empty_field_values(*this, field_values, "HSET")) {
            return nullptr;
        }

        auto cmd = make_cmd("hset", key);
        for (auto &[field, value] : field_values)
        {
            append_arg(cmd, field);
            append_arg(cmd, value);
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hmset(std::string key, const std::unordered_map<std::string, std::string> &field_values)
    {
        if (!require_non_empty_field_values(*this, field_values, "HMSET")) {
            return nullptr;
        }

        auto cmd = make_cmd("hmset", key);
        for (auto &[field, value] : field_values)
        {
            append_arg(cmd, field);
            append_arg(cmd, value);
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hmget(std::string key, const std::vector<std::string> &fields)
    {
        if (!require_non_empty_fields(*this, fields, "HMGET")) {
            return nullptr;
        }

        auto cmd = make_cmd("hmget", key);
        append_args(cmd, fields);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hgetall(std::string key)
    {
        auto cmd = make_cmd("hgetall", key);
        cmd->set_unpack_to_map(true);
        return impl_->execute_command(cmd);
    }
    std::shared_ptr<RedisValue> RedisClient::hdel(std::string key, const std::vector<std::string> &fields)
    {
        if (!require_non_empty_fields(*this, fields, "HDEL")) {
            return nullptr;
        }

        auto cmd = make_cmd("hdel", key);
        append_args(cmd, fields);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::hlen(std::string key)
    {
        return impl_->execute_command(make_cmd("hlen", key));
    }

    std::shared_ptr<RedisValue> RedisClient::hkeys(std::string key)
    {
        return impl_->execute_command(make_cmd("hkeys", key));
    }

    std::shared_ptr<RedisValue> RedisClient::hvals(std::string key)
    {
        return impl_->execute_command(make_cmd("hvals", key));
    }

    std::shared_ptr<RedisValue> RedisClient::hincrby(std::string key, std::string field, int64_t increment)
    {
        return impl_->execute_command(make_cmd("hincrby", key, field, increment));
    }

    std::shared_ptr<RedisValue> RedisClient::hincrbyfloat(std::string key, std::string field, double increment)
    {
        return impl_->execute_command(make_cmd("hincrbyfloat", key, field, serializeDouble(increment)));
    }

    std::shared_ptr<RedisValue> RedisClient::hexists(std::string key, std::string field)
    {
        return impl_->execute_command(make_cmd("hexists", key, field));
    }

    std::shared_ptr<RedisValue> RedisClient::hscan(std::string key, int64_t cursor, const std::string &match_pattern, int64_t count)
    {
        auto cmd = make_cmd("hscan", key, cursor);
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

    std::shared_ptr<RedisValue> RedisClient::hscan(std::string key, int64_t cursor, const std::vector<std::string> &match_patterns, int64_t count)
    {
        if (match_patterns.size() > 1)
        {
            set_last_error(ErrorValue::from_string("ERR: HSCAN supports at most one MATCH pattern"));
            return nullptr;
        }

        return hscan(std::move(key), cursor, match_patterns.empty() ? std::string() : match_patterns.front(), count);
    }
}
