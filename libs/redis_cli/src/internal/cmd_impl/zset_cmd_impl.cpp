#include "../redis_impl.h"
#include "../utils.h"
#include "internal/cmd_builder.h"
#include "redis_client.h"
#include "value/error_value.h"

#include <string>

namespace yuan::redis
{
    std::shared_ptr<RedisValue> RedisClient::zadd(std::string key, const std::unordered_map<std::string, double> &member_scores, bool nx /*= false*/, bool xx /*= false*/, bool ch /*= false*/, bool incr /*= false*/)
    {
        if (nx && xx) {
            set_last_error(ErrorValue::from_string("ERR: ZADD NX and XX cannot be used together"));
            return nullptr;
        }

        if (incr && member_scores.size() != 1) {
            set_last_error(ErrorValue::from_string("ERR: ZADD INCR supports exactly one member/score pair"));
            return nullptr;
        }

        auto cmd = make_cmd("zadd", key);

        if (nx) {
            append_arg(cmd, "NX");
        }

        if (xx) {
            append_arg(cmd, "XX");
        }

        if (ch) {
            append_arg(cmd, "CH");
        }

        if (incr) {
            append_arg(cmd, "INCR");
        }

        for (auto &[member, score] : member_scores) {
            append_arg(cmd, serializeDouble(score));
            append_arg(cmd, member);
        }

        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrem(std::string key, const std::vector<std::string> &members)
    {
        auto cmd = make_cmd("zrem", key);
        
        append_args(cmd, members);
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrange(std::string key, int64_t start, int64_t stop, bool with_scores /*= false*/)
    {
        auto cmd = make_cmd("zrange", key, start, stop);
        
        if (with_scores) {
            append_arg(cmd, "WITHSCORES");
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrevrange(std::string key, int64_t start, int64_t stop, bool with_scores /*= false*/)
    {
        auto cmd = make_cmd("zrevrange", key, start, stop);
        
        if (with_scores) {
            append_arg(cmd, "WITHSCORES");
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrangebyscore(std::string key, double min, double max, bool with_scores /*= false*/, bool min_open /*= false*/, bool max_open /*= false*/, int64_t offset /*= 0*/, int64_t count /*= -1*/)
    {
        auto cmd = make_cmd("zrangebyscore", key);
        
        append_arg(cmd, (min_open ? "(" : "") + serializeDouble(min));
        append_arg(cmd, (max_open ? "(" : "") + serializeDouble(max));
        
        if (offset != 0 && count < 0) {
            set_last_error(ErrorValue::from_string("ERR: ZRANGEBYSCORE offset requires a non-negative count"));
            return nullptr;
        }

        if (count >= 0) {
            append_arg(cmd, "LIMIT");
            append_arg(cmd, offset);
            append_arg(cmd, count);
        }

        if (with_scores) {
            append_arg(cmd, "WITHSCORES");
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrevrangebyscore(std::string key, double max, double min, bool with_scores /*= false*/, bool min_open /*= false*/, bool max_open /*= false*/, int64_t offset /*= 0*/, int64_t count /*= -1*/)
    {
        auto cmd = make_cmd("zrevrangebyscore", key);
        
        append_arg(cmd, (max_open ? "(" : "") + serializeDouble(max));
        append_arg(cmd, (min_open ? "(" : "") + serializeDouble(min));
        
        if (offset != 0 && count < 0) {
            set_last_error(ErrorValue::from_string("ERR: ZREVRANGEBYSCORE offset requires a non-negative count"));
            return nullptr;
        }

        if (count >= 0) {
            append_arg(cmd, "LIMIT");
            append_arg(cmd, offset);
            append_arg(cmd, count);
        }

        if (with_scores) {
            append_arg(cmd, "WITHSCORES");
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrank(std::string key, std::string member)
    {
        return impl_->execute_command(make_cmd("zrank", key, member));
    }

    std::shared_ptr<RedisValue> RedisClient::zrevrank(std::string key, std::string member)
    {
        return impl_->execute_command(make_cmd("zrevrank", key, member));
    }

    std::shared_ptr<RedisValue> RedisClient::zcard(std::string key)
    {
        return impl_->execute_command(make_cmd("zcard", key));
    }

    std::shared_ptr<RedisValue> RedisClient::zscore(std::string key, std::string member)
    {
        return impl_->execute_command(make_cmd("zscore", key, member));
    }

    std::shared_ptr<RedisValue> RedisClient::zincrby(std::string key, std::string member, double increment)
    {
        return impl_->execute_command(make_cmd("zincrby", key, serializeDouble(increment), member));
    }

    std::shared_ptr<RedisValue> RedisClient::zcount(std::string key, double min, double max, bool min_open /*= false*/, bool max_open /*= false*/)
    {
        auto cmd = make_cmd("zcount", key);
        
        append_arg(cmd, (min_open ? "(" : "") + serializeDouble(min));
        append_arg(cmd, (max_open ? "(" : "") + serializeDouble(max));
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zremrangebyrank(std::string key, int64_t start, int64_t stop)
    {
        return impl_->execute_command(make_cmd("zremrangebyrank", key, start, stop));
    }

    std::shared_ptr<RedisValue> RedisClient::zremrangebyscore(std::string key, double min, double max, bool min_open /*= false*/, bool max_open /*= false*/)
    {
        auto cmd = make_cmd("zremrangebyscore", key);

        append_arg(cmd, (min_open ? "(" : "") + serializeDouble(min));
        append_arg(cmd, (max_open ? "(" : "") + serializeDouble(max));
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zunionstore(std::string destination, const std::vector<std::string> &keys, const std::vector<double> &weights, const std::string &aggregate /*= "sum"*/)
    {
        auto cmd = make_cmd("zunionstore", destination, keys.size());
        
        append_args(cmd, keys);
        
        if (weights.size() > 0) {
            append_arg(cmd, "WEIGHTS");
            for (auto &weight : weights) {
                append_arg(cmd, serializeDouble(weight));
            }
        }
        
        if (!aggregate.empty()) {
            append_arg(cmd, "AGGREGATE");
            append_arg(cmd, aggregate);
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zinterstore(std::string destination, const std::vector<std::string> &keys, const std::vector<double> &weights, const std::string &aggregate /*= "sum"*/)
    {
        auto cmd = make_cmd("zinterstore", destination, keys.size());
        
        append_args(cmd, keys);
        
        if (weights.size() > 0) {
            append_arg(cmd, "WEIGHTS");
            for (auto &weight : weights) {
                append_arg(cmd, serializeDouble(weight));
            }
        }
        
        if (!aggregate.empty()) {
            append_arg(cmd, "AGGREGATE");
            append_arg(cmd, aggregate);
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zscan(std::string key, int64_t cursor, const std::string &match_pattern /*= "" */, int64_t count /*= 10*/)
    {
        auto cmd = make_cmd("zscan", key, cursor);
        
        if (match_pattern.size() > 0) {
            append_arg(cmd, "MATCH");
            append_arg(cmd, match_pattern);
        }
        
        if (count > 0) {
            append_arg(cmd, "COUNT");
            append_arg(cmd, count);
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zscan(std::string key, int64_t cursor, const std::vector<std::string> &match_patterns, int64_t count /*= 10*/)
    {
        if (match_patterns.size() > 1) {
            set_last_error(ErrorValue::from_string("ERR: ZSCAN supports at most one MATCH pattern"));
            return nullptr;
        }

        auto cmd = make_cmd("zscan", key, cursor);
        
        if (match_patterns.size() > 0) {
            append_arg(cmd, "MATCH");
            append_arg(cmd, match_patterns.front());
        }
        
        if (count > 0) {
            append_arg(cmd, "COUNT");
            append_arg(cmd, count);
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrangebylex(std::string key, std::string min, std::string max, bool min_open /*= false*/, bool max_open /*= false*/, int64_t offset /*= 0*/, int64_t count /*= -1*/)
    {
        auto cmd = make_cmd("zrangebylex",
            key,
            (min == "-" || min == "+") ? min : std::string(min_open ? "(" : "[") + min,
            (max == "-" || max == "+") ? max : std::string(max_open ? "(" : "[") + max);
        
        if (offset != 0 && count < 0) {
            set_last_error(ErrorValue::from_string("ERR: ZRANGEBYLEX offset requires a non-negative count"));
            return nullptr;
        }

        if (count >= 0) {
            append_arg(cmd, "LIMIT");
            append_arg(cmd, offset);
            append_arg(cmd, count);
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrevrangebylex(std::string key, std::string max, std::string min, bool min_open /*= false*/, bool max_open /*= false*/, int64_t offset /*= 0*/, int64_t count /*= -1*/)
    {
        auto cmd = make_cmd("zrevrangebylex",
            key,
            (max == "-" || max == "+") ? max : std::string(max_open ? "(" : "[") + max,
            (min == "-" || min == "+") ? min : std::string(min_open ? "(" : "[") + min);
        
        if (offset != 0 && count < 0) {
            set_last_error(ErrorValue::from_string("ERR: ZREVRANGEBYLEX offset requires a non-negative count"));
            return nullptr;
        }

        if (count >= 0) {
            append_arg(cmd, "LIMIT");
            append_arg(cmd, offset);
            append_arg(cmd, count);
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zlexcount(std::string key, std::string min, std::string max, bool min_open /*= false*/, bool max_open /*= false*/)
    {
        return impl_->execute_command(make_cmd("zlexcount",
            key,
            (min == "-" || min == "+") ? min : std::string(min_open ? "(" : "[") + min,
            (max == "-" || max == "+") ? max : std::string(max_open ? "(" : "[") + max));
    }

    std::shared_ptr<RedisValue> RedisClient::zremrangebylex(std::string key, std::string min, std::string max, bool min_open /*= false*/, bool max_open /*= false*/)
    {
        return impl_->execute_command(make_cmd("zremrangebylex",
            key,
            (min == "-" || min == "+") ? min : std::string(min_open ? "(" : "[") + min,
            (max == "-" || max == "+") ? max : std::string(max_open ? "(" : "[") + max));
    }

    std::shared_ptr<RedisValue> RedisClient::zpopmin(std::string key)
    {
        return impl_->execute_command(make_cmd("zpopmin", key));
    }

    std::shared_ptr<RedisValue> RedisClient::zpopmin(std::string key, int64_t count)
    {
        if (count <= 0) {
            set_last_error(ErrorValue::from_string("ERR: ZPOPMIN count must be greater than zero"));
            return nullptr;
        }

        return impl_->execute_command(make_cmd("zpopmin", key, count));
    }

    std::shared_ptr<RedisValue> RedisClient::zpopmax(std::string key)
    {
        return impl_->execute_command(make_cmd("zpopmax", key));
    }

    std::shared_ptr<RedisValue> RedisClient::zpopmax(std::string key, int64_t count)
    {
        if (count <= 0) {
            set_last_error(ErrorValue::from_string("ERR: ZPOPMAX count must be greater than zero"));
            return nullptr;
        }

        return impl_->execute_command(make_cmd("zpopmax", key, count));
    }

    std::shared_ptr<RedisValue> RedisClient::bzpopmin(std::string key, int timeout)
    {
        return impl_->execute_command(make_cmd("bzpopmin", key, timeout));
    }

    std::shared_ptr<RedisValue> RedisClient::bzpopmin(std::string key, int timeout, int64_t count)
    {
        if (count != 1) {
            set_last_error(ErrorValue::from_string("ERR: BZPOPMIN does not support count; use the two-argument overload"));
            return nullptr;
        }

        return bzpopmin(std::move(key), timeout);
    }

    std::shared_ptr<RedisValue> RedisClient::bzpopmax(std::string key, int timeout)
    {
        return impl_->execute_command(make_cmd("bzpopmax", key, timeout));
    }

    std::shared_ptr<RedisValue> RedisClient::bzpopmax(std::string key, int timeout, int64_t count)
    {
        if (count != 1) {
            set_last_error(ErrorValue::from_string("ERR: BZPOPMAX does not support count; use the two-argument overload"));
            return nullptr;
        }

        return bzpopmax(std::move(key), timeout);
    }

    std::shared_ptr<RedisValue> RedisClient::zrandmember(std::string key, int count, bool with_scores /*= false*/)
    {
        auto cmd = make_cmd("zrandmember", key, count);
        
        if (with_scores) {
            append_arg(cmd, "WITHSCORES");
        }
        
        return impl_->execute_command(cmd);
    }
}
