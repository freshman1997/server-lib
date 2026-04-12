#ifndef __YUAN_REDIS_INTERNAL_CMD_BUILDER_H__
#define __YUAN_REDIS_INTERNAL_CMD_BUILDER_H__

#include "cmd/default_cmd.h"
#include "redis_value.h"
#include "value/string_value.h"

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace yuan::redis
{

inline std::shared_ptr<RedisValue> redis_arg(const std::shared_ptr<RedisValue> &value)
{
    return value;
}

inline std::shared_ptr<RedisValue> redis_arg(const std::string &value)
{
    return StringValue::from_string(value);
}

inline std::shared_ptr<RedisValue> redis_arg(std::string_view value)
{
    return StringValue::from_string(std::string(value));
}

inline std::shared_ptr<RedisValue> redis_arg(const char *value)
{
    return StringValue::from_string(value ? std::string(value) : std::string());
}

template <typename T>
std::enable_if_t<std::is_arithmetic_v<std::decay_t<T>> && !std::is_same_v<std::decay_t<T>, bool>, std::shared_ptr<RedisValue>>
redis_arg(T value)
{
    return StringValue::from_string(std::to_string(value));
}

inline void append_arg(const std::shared_ptr<DefaultCmd> &cmd, const std::shared_ptr<RedisValue> &value)
{
    cmd->add_arg(value);
}

template <typename T>
void append_arg(const std::shared_ptr<DefaultCmd> &cmd, T &&value)
{
    cmd->add_arg(redis_arg(std::forward<T>(value)));
}

inline void append_args(const std::shared_ptr<DefaultCmd> &cmd, const std::vector<std::string> &values)
{
    for (const auto &value : values) {
        append_arg(cmd, value);
    }
}

inline std::vector<std::shared_ptr<RedisValue>> make_args()
{
    return {};
}

inline std::shared_ptr<DefaultCmd> make_cmd(const std::string &name, const std::vector<std::shared_ptr<RedisValue>> &args)
{
    auto cmd = std::make_shared<DefaultCmd>();
    cmd->set_args(name, args);
    return cmd;
}

template <typename... Args>
std::vector<std::shared_ptr<RedisValue>> make_args(Args &&...args)
{
    std::vector<std::shared_ptr<RedisValue>> values;
    values.reserve(sizeof...(Args));
    (values.push_back(redis_arg(std::forward<Args>(args))), ...);
    return values;
}

template <typename... Args>
std::shared_ptr<DefaultCmd> make_cmd(const std::string &name, Args &&...args)
{
    auto cmd = std::make_shared<DefaultCmd>();
    cmd->set_args(name, make_args(std::forward<Args>(args)...));
    return cmd;
}

} // namespace yuan::redis

#endif // __YUAN_REDIS_INTERNAL_CMD_BUILDER_H__
