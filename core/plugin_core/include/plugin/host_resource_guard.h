#ifndef __YUAN_PLUGIN_HOST_RESOURCE_GUARD_H__
#define __YUAN_PLUGIN_HOST_RESOURCE_GUARD_H__

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace yuan::plugin
{

/// 资源清理回调类型
using ResourceCleanupFn = std::function<void()>;

/// 资源类型枚举 (用于日志和审计)
enum class PluginResourceType : uint8_t
{
    event_subscription,
    scheduler_task,
    service_registration,
    http_middleware,
    http_route,
    callback,
    coroutine_task,
    async_task,
    custom,
};

inline const char *to_string(PluginResourceType type)
{
    switch (type) {
    case PluginResourceType::event_subscription: return "event_subscription";
    case PluginResourceType::scheduler_task: return "scheduler_task";
    case PluginResourceType::service_registration: return "service_registration";
    case PluginResourceType::http_middleware: return "http_middleware";
    case PluginResourceType::http_route: return "http_route";
    case PluginResourceType::callback: return "callback";
    case PluginResourceType::coroutine_task: return "coroutine_task";
    case PluginResourceType::async_task: return "async_task";
    case PluginResourceType::custom: return "custom";
    default: return "unknown";
    }
}

/// 插件资源守卫 — 追踪插件注册的所有宿主资源, 卸载时自动清理
///
/// 使用方式:
///   1. 宿主接口在执行插件请求时, 调用 track() 记录清理回调
///   2. 插件可主动调用 untrack() 移除已手动清理的资源
///   3. 卸载时调用 cleanup_all() 自动执行所有清理回调
class HostResourceGuard
{
public:
    virtual ~HostResourceGuard() = default;

    /// 追踪一个资源, 返回资源 ID; cleanup 回调在 cleanup_all() 时执行
    virtual uint64_t track(const std::string &plugin_name,
                           PluginResourceType type,
                           ResourceCleanupFn cleanup,
                           const std::string &description = "") = 0;

    /// 取消追踪 (插件已自行清理的资源)
    virtual bool untrack(uint64_t resource_id) = 0;

    /// 清理指定插件的所有已追踪资源 (卸载时调用)
    virtual void cleanup_plugin(const std::string &plugin_name) = 0;

    /// 清理所有已追踪资源 (进程退出时调用)
    virtual void cleanup_all() = 0;

    /// 查询指定插件已追踪的资源数量
    virtual size_t tracked_count(const std::string &plugin_name) const = 0;

    /// 查询指定插件是否有已追踪的资源
    virtual bool has_tracked_resources(const std::string &plugin_name) const = 0;
};

} // namespace yuan::plugin

#endif
