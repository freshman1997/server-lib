#include "redis_client.h"
#include "../redis_impl.h"
#include "cmd/subcribe_cmd.h"
#include "internal/cmd_builder.h"
#include "value/array_value.h"
#include "value/error_value.h"
#include "value/int_value.h"
#include "value/string_value.h"

namespace yuan::redis
{
    namespace
    {
        std::shared_ptr<RedisValue> combine_results(const std::vector<std::shared_ptr<RedisValue> > &results)
        {
            if (results.empty()) {
                return nullptr;
            }

            if (results.size() == 1) {
                return results.front();
            }

            auto combined = std::make_shared<ArrayValue>();
            for (const auto &result : results) {
                combined->add_value(result ? result : ErrorValue::from_string("null result"));
            }
            return combined;
        }

        bool apply_unsubscribe_frame(
            const std::shared_ptr<SubcribeCmd> &subscribe_cmd,
            const std::shared_ptr<RedisValue> &frame_value,
            int64_t &remaining)
        {
            if (!subscribe_cmd) {
                return false;
            }

            if (!frame_value || frame_value->get_type() != resp_array) {
                return false;
            }

            const auto arr = frame_value->as<ArrayValue>();
            if (!arr) {
                return false;
            }

            const auto &values = arr->get_values();
            if (values.size() < 3) {
                return false;
            }

            const auto kind = values[0] ? values[0]->as<StringValue>() : nullptr;
            if (!kind) {
                return false;
            }

            const auto count = values[2] ? values[2]->as<IntValue>() : nullptr;
            if (!count) {
                return false;
            }

            const auto target = values[1] ? values[1]->as<StringValue>() : nullptr;
            const std::vector<std::string> targets = target && !target->get_value().empty()
                ? std::vector<std::string>{ target->get_value() }
                : std::vector<std::string>{};

            if (kind->get_value() == "punsubscribe") {
                subscribe_cmd->punsubcribe(targets);
            } else if (kind->get_value() == "unsubscribe") {
                subscribe_cmd->unsubcribe(targets);
            } else {
                return false;
            }

            remaining = count->get_value();
            return true;
        }

        void sync_unsubscribe_state(
            std::shared_ptr<SubcribeCmd> &subscribe_cmd,
            const std::shared_ptr<RedisValue> &result)
        {
            if (!subscribe_cmd || !result || result->get_type() != resp_array) {
                return;
            }

            const auto arr = result->as<ArrayValue>();
            if (!arr) {
                return;
            }

            bool synced = false;
            int64_t remaining = 0;
            const auto &values = arr->get_values();
            if (!values.empty() && values.front() && values.front()->get_type() == resp_array) {
                for (const auto &frame : values) {
                    synced = apply_unsubscribe_frame(subscribe_cmd, frame, remaining) || synced;
                }
            } else {
                synced = apply_unsubscribe_frame(subscribe_cmd, result, remaining);
            }

            if (synced && (remaining <= 0 || !subscribe_cmd->is_subcribe())) {
                subscribe_cmd = nullptr;
            }
        }
    }

    std::shared_ptr<RedisValue> RedisClient::publish(std::string channel, std::string message)
    {
        return impl_->execute_command(make_cmd("publish", channel, message));
    }

    std::shared_ptr<RedisValue> RedisClient::subscribe(const std::vector<std::string> & channels, std::function<void(const std::vector<SubMessage> & messages)> msg_callback)
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->operation_mutex_);

        if (channels.empty()) {
            impl_->last_error_ = ErrorValue::from_string("channels are empty");
            return nullptr;
        }

        auto cmd = impl_->subcribe_cmd ? impl_->subcribe_cmd : std::make_shared<SubcribeCmd>();
        cmd->set_max_pending_messages(impl_->option_.max_pubsub_pending_messages_);
        cmd->set_args("subscribe", make_args());
        if (msg_callback) {
            cmd->set_msg_callback(std::move(msg_callback));
        }

        append_args(cmd, channels);

        impl_->subcribe_cmd = cmd;
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::psubscribe(const std::vector<std::string> & patterns, std::function<void(const std::vector<PSubMessage> & messages)> pmsg_callback)
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->operation_mutex_);

        if (patterns.empty()) {
            impl_->last_error_ = ErrorValue::from_string("patterns are empty");
            return nullptr;
        }

        auto cmd = impl_->subcribe_cmd ? impl_->subcribe_cmd : std::make_shared<SubcribeCmd>();
        cmd->set_max_pending_messages(impl_->option_.max_pubsub_pending_messages_);
        cmd->set_args("psubscribe", make_args());
        if (pmsg_callback) {
            cmd->set_pmsg_callback(std::move(pmsg_callback));
        }

        append_args(cmd, patterns);

        impl_->subcribe_cmd = cmd;

        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::unsubscribe(const std::vector<std::string> & channels)
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->operation_mutex_);

        auto cmd = make_cmd("unsubscribe");

        append_args(cmd, channels);

        auto result = impl_->execute_command(cmd);
        sync_unsubscribe_state(impl_->subcribe_cmd, result);
        return result;
    }

    std::shared_ptr<RedisValue> RedisClient::punsubscribe(const std::vector<std::string> & patterns)
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->operation_mutex_);

        auto cmd = make_cmd("punsubscribe");

        append_args(cmd, patterns);

        auto result = impl_->execute_command(cmd);
        sync_unsubscribe_state(impl_->subcribe_cmd, result);
        return result;
    }

    std::shared_ptr<RedisValue> RedisClient::subscribe_mixed(const std::vector<std::string> & channels, const std::vector<std::string> & patterns)
    {
        std::vector<std::shared_ptr<RedisValue> > results;

        if (!patterns.empty()) {
            auto result = psubscribe(patterns, std::function<void(const std::vector<PSubMessage> & messages)>{});
            if (!result) {
                return nullptr;
            }
            results.push_back(result);
        }

        if (!channels.empty()) {
            auto result = subscribe(channels, std::function<void(const std::vector<SubMessage> & messages)>{});
            if (!result) {
                return nullptr;
            }
            results.push_back(result);
        }

        if (results.empty()) {
            impl_->last_error_ = ErrorValue::from_string("patterns and channels are empty");
            return nullptr;
        }

        return combine_results(results);
    }

    std::shared_ptr<RedisValue> RedisClient::unsubscribe_mixed(const std::vector<std::string> & channels, const std::vector<std::string> & patterns)
    {
        std::vector<std::shared_ptr<RedisValue> > results;

        if (!channels.empty()) {
            auto result = unsubscribe(channels);
            if (!result) {
                return nullptr;
            }
            results.push_back(result);
        }

        if (!patterns.empty()) {
            auto result = punsubscribe(patterns);
            if (!result) {
                return nullptr;
            }
            results.push_back(result);
        }

        if (results.empty()) {
            impl_->last_error_ = ErrorValue::from_string("channels and patterns are empty");
            return nullptr;
        }

        return combine_results(results);
    }

    std::shared_ptr<RedisValue> RedisClient::update_subscriptions(
        const std::vector<std::string> & subscribe_channels,
        const std::vector<std::string> & subscribe_patterns,
        const std::vector<std::string> & unsubscribe_channels,
        const std::vector<std::string> & unsubscribe_patterns)
    {
        std::vector<std::shared_ptr<RedisValue> > results;

        if (!subscribe_channels.empty() || !subscribe_patterns.empty()) {
            auto result = subscribe_mixed(subscribe_channels, subscribe_patterns);
            if (!result) {
                return nullptr;
            }
            results.push_back(result);
        }

        if (!unsubscribe_patterns.empty()) {
            auto result = punsubscribe(unsubscribe_patterns);
            if (!result) {
                return nullptr;
            }
            results.push_back(result);
        }

        if (!unsubscribe_channels.empty()) {
            auto result = unsubscribe(unsubscribe_channels);
            if (!result) {
                return nullptr;
            }
            results.push_back(result);
        }

        if (results.empty()) {
            impl_->last_error_ = ErrorValue::from_string("subscription update arguments are empty");
            return nullptr;
        }

        return combine_results(results);
    }
}
