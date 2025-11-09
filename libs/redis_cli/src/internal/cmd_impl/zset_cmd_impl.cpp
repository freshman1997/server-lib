#include "../redis_impl.h"
#include "../utils.h"
#include "cmd/default_cmd.h"
#include "redis_client.h"
#include "value/float_value.h"
#include "value/int_value.h"
#include "value/string_value.h"

#include <string>

namespace yuan::redis
{
    std::shared_ptr<RedisValue> RedisClient::zadd(std::string key, const std::unordered_map<std::string, double> &member_scores, bool nx /*= false*/, bool xx /*= false*/, bool ch /*= false*/, bool incr /*= false*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zadd", {std::make_shared<StringValue>(key)});

        if (nx) {
            cmd->add_arg(std::make_shared<StringValue>("NX"));
        }

        if (xx) {
            cmd->add_arg(std::make_shared<StringValue>("XX"));
        }

        if (ch) {
            cmd->add_arg(std::make_shared<StringValue>("CH"));
        }

        if (incr) {
            cmd->add_arg(std::make_shared<StringValue>("INCR"));
        }

        for (auto &[member, score] : member_scores) {
            cmd->add_arg(std::make_shared<FloatValue>(score));
            cmd->add_arg(std::make_shared<StringValue>(member));
        }

        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrem(std::string key, const std::vector<std::string> &members)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zrem", {std::make_shared<StringValue>(key)});
        
        for (auto &member : members) {
            cmd->add_arg(std::make_shared<StringValue>(member));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrange(std::string key, int64_t start, int64_t stop, bool with_scores /*= false*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zrange", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(start)), std::make_shared<StringValue>(std::to_string(stop))});
        
        if (with_scores) {
            cmd->add_arg(std::make_shared<StringValue>("WITHSCORES"));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrevrange(std::string key, int64_t start, int64_t stop, bool with_scores /*= false*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zrevrange", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(start)), std::make_shared<StringValue>(std::to_string(stop))});
        
        if (with_scores) {
            cmd->add_arg(std::make_shared<StringValue>("WITHSCORES"));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrangebyscore(std::string key, double min, double max, bool with_scores /*= false*/, bool min_open /*= false*/, bool max_open /*= false*/, int64_t offset /*= 0*/, int64_t count /*= 10*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zrangebyscore", {std::make_shared<StringValue>(key)});
        
        if (min_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }

        cmd->add_arg(std::make_shared<FloatValue>(min));
        
        if (max_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }

        cmd->add_arg(std::make_shared<FloatValue>(max));
        
        if (offset > 0) {
            cmd->add_arg(std::make_shared<StringValue>("LIMIT"));
            cmd->add_arg(std::make_shared<IntValue>(offset));
            cmd->add_arg(std::make_shared<IntValue>(count));
        }

        if (with_scores) {
            cmd->add_arg(std::make_shared<StringValue>("WITHSCORES"));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrevrangebyscore(std::string key, double max, double min, bool with_scores /*= false*/, bool min_open /*= false*/, bool max_open /*= false*/, int64_t offset /*= 0*/, int64_t count /*= 10*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zrevrangebyscore", {std::make_shared<StringValue>(key)});
        
        if (with_scores) {
            cmd->add_arg(std::make_shared<StringValue>("WITHSCORES"));
        }
        
        if (min_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }

        cmd->add_arg(std::make_shared<StringValue>(serializeDouble(min)));
        
        if (max_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }

        cmd->add_arg(std::make_shared<StringValue>(serializeDouble(max)));
        
        if (offset > 0) {
            cmd->add_arg(std::make_shared<StringValue>("LIMIT"));
            cmd->add_arg(std::make_shared<StringValue>(std::to_string(offset)));
            cmd->add_arg(std::make_shared<StringValue>(std::to_string(count)));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrank(std::string key, std::string member)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zrank", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(member)});
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrevrank(std::string key, std::string member)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zrevrank", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(member)});
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zcard(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zcard", {std::make_shared<StringValue>(key)});
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zscore(std::string key, std::string member)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zscore", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(member)});
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zincrby(std::string key, std::string member, double increment)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zincrby", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(member), std::make_shared<StringValue>(serializeDouble(increment))});
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zcount(std::string key, double min, double max, bool min_open /*= false*/, bool max_open /*= false*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zcount", {std::make_shared<StringValue>(key)});
        
        if (min_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }

        cmd->add_arg(std::make_shared<StringValue>(serializeDouble(min)));
        
        if (max_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }

        cmd->add_arg(std::make_shared<StringValue>(serializeDouble(max)));
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zremrangebyrank(std::string key, int64_t start, int64_t stop)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zremrangebyrank", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(start)), std::make_shared<StringValue>(std::to_string(stop))});
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zremrangebyscore(std::string key, double min, double max, bool min_open /*= false*/, bool max_open /*= false*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zremrangebyscore", {std::make_shared<StringValue>(key)});
        
        if (min_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }

        cmd->add_arg(std::make_shared<StringValue>(serializeDouble(min)));

        
        if (max_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }

        cmd->add_arg(std::make_shared<StringValue>(serializeDouble(max)));
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zunionstore(std::string destination, const std::vector<std::string> &keys, const std::vector<double> &weights, const std::string &aggregate /*= "sum"*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zunionstore", {std::make_shared<StringValue>(destination), std::make_shared<StringValue>(std::to_string(keys.size()))});
        
        for (auto &key : keys) {
            cmd->add_arg(std::make_shared<StringValue>(key));
        }
        
        if (weights.size() > 0) {
            cmd->add_arg(std::make_shared<StringValue>("WEIGHTS"));
            for (auto &weight : weights) {
                cmd->add_arg(std::make_shared<StringValue>(serializeDouble(weight)));
            }
        }
        
        if (!aggregate.empty()) {
            cmd->add_arg(std::make_shared<StringValue>("AGGREGATE"));
            cmd->add_arg(std::make_shared<StringValue>(aggregate));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zinterstore(std::string destination, const std::vector<std::string> &keys, const std::vector<double> &weights, const std::string &aggregate /*= "sum"*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zinterstore", {std::make_shared<StringValue>(destination), std::make_shared<StringValue>(std::to_string(keys.size()))});
        
        for (auto &key : keys) {
            cmd->add_arg(std::make_shared<StringValue>(key));
        }
        
        if (weights.size() > 0) {
            cmd->add_arg(std::make_shared<StringValue>("WEIGHTS"));
            for (auto &weight : weights) {
                cmd->add_arg(std::make_shared<StringValue>(serializeDouble(weight)));
            }
        }
        
        if (!aggregate.empty()) {
            cmd->add_arg(std::make_shared<StringValue>("AGGREGATE"));
            cmd->add_arg(std::make_shared<StringValue>(aggregate));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zscan(std::string key, int64_t cursor, const std::string &match_pattern /*= "" */, int64_t count /*= 10*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zscan", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(cursor))});
        
        if (match_pattern.size() > 0) {
            cmd->add_arg(std::make_shared<StringValue>("MATCH"));
            cmd->add_arg(std::make_shared<StringValue>(match_pattern));
        }
        
        if (count > 0) {
            cmd->add_arg(std::make_shared<StringValue>("COUNT"));
            cmd->add_arg(std::make_shared<StringValue>(std::to_string(count)));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zscan(std::string key, int64_t cursor, const std::vector<std::string> &match_patterns, int64_t count /*= 10*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zscan", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(cursor))});
        
        if (match_patterns.size() > 0) {
            cmd->add_arg(std::make_shared<StringValue>("MATCH"));
            for (auto &pattern : match_patterns) {
                cmd->add_arg(std::make_shared<StringValue>(pattern));
            }
        }
        
        if (count > 0) {
            cmd->add_arg(std::make_shared<StringValue>("COUNT"));
            cmd->add_arg(std::make_shared<StringValue>(std::to_string(count)));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrangebylex(std::string key, std::string min, std::string max, bool min_open /*= false*/, bool max_open /*= false*/, int64_t offset /*= 0*/, int64_t count /*= 10*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zrangebylex", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(min), std::make_shared<StringValue>(max)});
        
        if (min_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }
        
        if (max_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }
        
        if (offset > 0) {
            cmd->add_arg(std::make_shared<StringValue>("LIMIT"));
            cmd->add_arg(std::make_shared<StringValue>(std::to_string(offset)));
            cmd->add_arg(std::make_shared<StringValue>(std::to_string(count)));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrevrangebylex(std::string key, std::string max, std::string min, bool min_open /*= false*/, bool max_open /*= false*/, int64_t offset /*= 0*/, int64_t count /*= 10*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zrevrangebylex", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(min), std::make_shared<StringValue>(max)});
        
        if (min_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }
        
        if (max_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }
        
        if (offset > 0) {
            cmd->add_arg(std::make_shared<StringValue>("LIMIT"));
            cmd->add_arg(std::make_shared<StringValue>(std::to_string(offset)));
            cmd->add_arg(std::make_shared<StringValue>(std::to_string(count)));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zlexcount(std::string key, std::string min, std::string max, bool min_open /*= false*/, bool max_open /*= false*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zlexcount", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(min), std::make_shared<StringValue>(max)});
        
        if (min_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }
        
        if (max_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zremrangebylex(std::string key, std::string min, std::string max, bool min_open /*= false*/, bool max_open /*= false*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zremrangebylex", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(min), std::make_shared<StringValue>(max)});
        
        if (min_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }
        
        if (max_open) {
            cmd->add_arg(std::make_shared<StringValue>("("));
        }
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zpopmin(std::string key, int64_t count)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zpopmin", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(count))});
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zpopmax(std::string key, int64_t count)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zpopmax", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(count))});
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bzpopmin(std::string key, int timeout, int64_t count)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("bzpopmin", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(timeout)), std::make_shared<StringValue>(std::to_string(count))});
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bzpopmax(std::string key, int timeout, int64_t count)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("bzpopmax", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(timeout)), std::make_shared<StringValue>(std::to_string(count))});
        
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::zrandmember(std::string key, int count, bool with_scores /*= false*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("zrandmember", {std::make_shared<StringValue>(key), std::make_shared<StringValue>(std::to_string(count))});
        
        if (with_scores) {
            cmd->add_arg(std::make_shared<StringValue>("WITHSCORES"));
        }
        
        return impl_->execute_command(cmd);
    }
}