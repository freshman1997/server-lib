#include "plugin_redis_storage.h"

#include "internal/def.h"
#include "logger.h"
#include "option.h"
#include "redis_client.h"
#include "redis_value.h"
#include "value/error_value.h"
#include "value/int_value.h"
#include "value/map_value.h"
#include "value/status_value.h"
#include "value/string_value.h"

namespace yuan::app
{

PluginRedisStorage::PluginRedisStorage(const std::string &plugin_name)
    : plugin_name_(plugin_name)
{
}

PluginRedisStorage::~PluginRedisStorage()
{
    if (client_ && client_->is_connected()) {
        client_->close();
    }
}

bool PluginRedisStorage::init()
{
    if (initialized_) {
        return true;
    }

    try {
        yuan::redis::Option opt;
        opt.host_ = config_.host;
        opt.port_ = config_.port;
        opt.password_ = config_.password;
        opt.db_ = config_.db;
        opt.name_ = "plugin_" + plugin_name_;

        client_ = std::make_shared<yuan::redis::RedisClient>(opt);
        if (!client_) {
            LOG_ERROR("plugin redis storage: failed to create redis client for '{}'", plugin_name_);
            return false;
        }

        auto ping = client_->ping();
        if (client_->get_last_error() || !ping || !client_->is_connected()) {
            LOG_ERROR("plugin redis storage: failed to connect redis for '{}'", plugin_name_);
            client_.reset();
            return false;
        }

        initialized_ = true;
        LOG_INFO("plugin redis storage initialized for '{}'", plugin_name_);
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR("plugin redis storage init failed for '{}': {}", plugin_name_, e.what());
        return false;
    }
}

std::string PluginRedisStorage::make_key(const std::string &key) const
{
    const auto &prefix = config_.key_prefix.empty() ? "yuan:plugin:" : config_.key_prefix;
    return prefix + plugin_name_ + ":" + key;
}

static bool is_redis_error(const std::shared_ptr<yuan::redis::RedisValue> &val)
{
    return !val || val->get_type() == yuan::redis::resp_error;
}

static bool is_redis_null(const std::shared_ptr<yuan::redis::RedisValue> &val)
{
    if (!val) {
        return true;
    }
    return val->get_type() == 0;
}

bool PluginRedisStorage::set(const std::string &key, const std::string &value)
{
    if (!is_available()) {
        return false;
    }
    auto result = client_->set(make_key(key), value);
    if (is_redis_error(result)) {
        return false;
    }
    if (auto status = result->as<yuan::redis::StatusValue>()) {
        return status->is_ok();
    }
    return true;
}

bool PluginRedisStorage::set(const std::string &key,
                             const std::string &value,
                             std::chrono::milliseconds ttl)
{
    if (!is_available()) {
        return false;
    }
    auto result = client_->set(make_key(key), value, static_cast<int>(ttl.count() / 1000));
    if (is_redis_error(result)) {
        return false;
    }
    if (auto status = result->as<yuan::redis::StatusValue>()) {
        return status->is_ok();
    }
    return true;
}

std::optional<std::string> PluginRedisStorage::get(const std::string &key)
{
    if (!is_available()) {
        return std::nullopt;
    }
    auto result = client_->get(make_key(key));
    if (is_redis_error(result) || is_redis_null(result)) {
        return std::nullopt;
    }
    if (auto str_val = result->as<yuan::redis::StringValue>()) {
        return str_val->get_value();
    }
    return result->to_string();
}

bool PluginRedisStorage::del(const std::string &key)
{
    if (!is_available()) {
        return false;
    }
    auto result = client_->del({make_key(key)});
    if (is_redis_error(result)) {
        return false;
    }
    if (auto int_val = result->as<yuan::redis::IntValue>()) {
        return int_val->get_value() > 0;
    }
    return true;
}

bool PluginRedisStorage::exists(const std::string &key)
{
    if (!is_available()) {
        return false;
    }
    auto result = client_->exists({make_key(key)});
    if (is_redis_error(result)) {
        return false;
    }
    if (auto int_val = result->as<yuan::redis::IntValue>()) {
        return int_val->get_value() > 0;
    }
    return false;
}

bool PluginRedisStorage::hset(const std::string &key,
                              const std::string &field,
                              const std::string &value)
{
    if (!is_available()) {
        return false;
    }
    auto result = client_->hset(make_key(key), field, value);
    return !is_redis_error(result);
}

std::optional<std::string> PluginRedisStorage::hget(const std::string &key, const std::string &field)
{
    if (!is_available()) {
        return std::nullopt;
    }
    auto result = client_->hget(make_key(key), field);
    if (is_redis_error(result) || is_redis_null(result)) {
        return std::nullopt;
    }
    if (auto str_val = result->as<yuan::redis::StringValue>()) {
        return str_val->get_value();
    }
    return result->to_string();
}

bool PluginRedisStorage::hdel(const std::string &key, const std::string &field)
{
    if (!is_available()) {
        return false;
    }
    auto result = client_->hdel(make_key(key), {field});
    if (is_redis_error(result)) {
        return false;
    }
    if (auto int_val = result->as<yuan::redis::IntValue>()) {
        return int_val->get_value() > 0;
    }
    return true;
}

std::unordered_map<std::string, std::string> PluginRedisStorage::hgetall(const std::string &key)
{
    if (!is_available()) {
        return {};
    }
    auto result = client_->hgetall(make_key(key));
    if (is_redis_error(result)) {
        return {};
    }

    std::unordered_map<std::string, std::string> out;
    if (auto map_val = result->as<yuan::redis::MapValue>()) {
        for (const auto &[k, v] : map_val->get_map_value()) {
            out[k] = v ? v->to_string() : "";
        }
    }
    return out;
}

bool PluginRedisStorage::is_available() const
{
    return initialized_ && client_ && client_->is_connected();
}

} // namespace yuan::app
