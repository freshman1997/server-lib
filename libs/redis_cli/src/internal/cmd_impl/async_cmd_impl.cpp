#include "../redis_impl.h"
#include "redis_client.h"
#include "internal/redis_registry.h"

namespace yuan::redis
{
    std::shared_ptr<RedisValue> RedisClient::command(
        const std::string &name, const std::vector<std::string> &args)
    {
        std::vector<PipelineCommand> cmds;
        cmds.emplace_back(name, args);
        auto result = pipeline(cmds);
        if (!result || result->get_type() != resp_array) {
            return result;
        }
        auto arr = result->as<ArrayValue>();
        if (!arr || arr->get_values().empty()) {
            return result;
        }
        return arr->get_values()[0];
    }

    SimpleTask<std::shared_ptr<RedisValue>> RedisClient::command_async(
        const std::string &name, const std::vector<std::string> &args)
    {
        auto runtime = impl_->registry()->get_coroutine_runtime();
        if (!runtime.event_loop()) {
            std::vector<PipelineCommand> cmds;
            cmds.emplace_back(name, args);
            auto result = pipeline(cmds);
            if (!result || result->get_type() != resp_array) {
                co_return result;
            }
            auto arr = result->as<ArrayValue>();
            if (!arr || arr->get_values().empty()) {
                co_return result;
            }
            co_return arr->get_values()[0];
        }

        co_await runtime.schedule();
        std::vector<PipelineCommand> cmds;
        cmds.emplace_back(name, args);
        auto result = pipeline(cmds);
        if (!result || result->get_type() != resp_array) {
            co_return result;
        }
        auto arr = result->as<ArrayValue>();
        if (!arr || arr->get_values().empty()) {
            co_return result;
        }
        co_return arr->get_values()[0];
    }

    SimpleTask<std::shared_ptr<RedisValue>> RedisClient::ping_async()
    {
        auto runtime = impl_->registry()->get_coroutine_runtime();
        if (!runtime.event_loop()) {
            co_return ping();
        }

        co_await runtime.schedule();
        co_return ping();
    }

    SimpleTask<std::shared_ptr<RedisValue>> RedisClient::get_async(std::string key)
    {
        auto runtime = impl_->registry()->get_coroutine_runtime();
        if (!runtime.event_loop()) {
            co_return get(key);
        }

        co_await runtime.schedule();
        co_return get(std::move(key));
    }

    SimpleTask<std::shared_ptr<RedisValue>> RedisClient::set_async(std::string key, std::string value)
    {
        auto runtime = impl_->registry()->get_coroutine_runtime();
        if (!runtime.event_loop()) {
            co_return set(key, value);
        }

        co_await runtime.schedule();
        co_return set(std::move(key), std::move(value));
    }

    SimpleTask<std::shared_ptr<RedisValue>> RedisClient::del_async(const std::vector<std::string> &keys)
    {
        auto runtime = impl_->registry()->get_coroutine_runtime();
        if (!runtime.event_loop()) {
            co_return del(keys);
        }

        co_await runtime.schedule();
        co_return del(keys);
    }

    SimpleTask<std::shared_ptr<RedisValue>> RedisClient::incr_async(std::string key)
    {
        auto runtime = impl_->registry()->get_coroutine_runtime();
        if (!runtime.event_loop()) {
            co_return incr(key);
        }

        co_await runtime.schedule();
        co_return incr(std::move(key));
    }
}