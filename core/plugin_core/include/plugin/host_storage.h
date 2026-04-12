#ifndef __YUAN_PLUGIN_HOST_STORAGE_H__
#define __YUAN_PLUGIN_HOST_STORAGE_H__

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::plugin
{

/// 宿主提供的持久化存储接口 (KV 模型)
///
/// 设计原则:
/// - 插件只依赖此抽象接口, 不依赖具体存储实现
/// - 宿主可替换底层实现 (Redis / SQLite / 文件 / 内存)
/// - key 自动添加插件前缀隔离, 防止冲突
class HostStorage
{
public:
    virtual ~HostStorage() = default;

    // ---- 基础 KV 操作 ----

    /// 设置键值 (覆盖写)
    virtual bool set(const std::string &key, const std::string &value) = 0;

    /// 设置键值带过期时间
    virtual bool set(const std::string &key, const std::string &value,
                     std::chrono::milliseconds ttl) = 0;

    /// 获取键值, 不存在返回 nullopt
    virtual std::optional<std::string> get(const std::string &key) = 0;

    /// 删除键
    virtual bool del(const std::string &key) = 0;

    /// 检查键是否存在
    virtual bool exists(const std::string &key) = 0;

    // ---- Hash 操作 ----

    /// 设置 hash field
    virtual bool hset(const std::string &key, const std::string &field, const std::string &value) = 0;

    /// 获取 hash field
    virtual std::optional<std::string> hget(const std::string &key, const std::string &field) = 0;

    /// 删除 hash field
    virtual bool hdel(const std::string &key, const std::string &field) = 0;

    /// 获取 hash 所有字段
    virtual std::unordered_map<std::string, std::string> hgetall(const std::string &key) = 0;

    // ---- 辅助 ----

    /// 检查存储后端是否可用
    virtual bool is_available() const = 0;

    /// 获取存储后端名称 (如 "redis", "memory", "sqlite")
    virtual const char *backend_name() const = 0;
};

} // namespace yuan::plugin

#endif
