#ifndef __YUAN_PLUGIN_PLUGIN_PERMISSION_H__
#define __YUAN_PLUGIN_PLUGIN_PERMISSION_H__

#include <string>
#include <vector>
#include <cstdint>

namespace yuan::plugin
{

    /// 插件可申请的权限位
    enum class PluginPermission : uint32_t {
        none = 0,
        use_event_bus = 1 << 0,        ///< 订阅/发布事件
        use_logger = 1 << 1,           ///< 写日志
        use_scheduler = 1 << 2,        ///< 注册定时任务
        use_service_catalog = 1 << 3,  ///< 查询宿主服务
        use_service_registry = 1 << 4, ///< 注册/注销插件服务
        use_http_intercept = 1 << 5,   ///< 注册 HTTP 中间件/路由
        use_storage = 1 << 6,          ///< 使用持久化存储
        use_network_runtime = 1 << 7,  ///< 使用网络运行时 (定时器/调度)
        use_extension_points = 1 << 8, ///< 解析扩展点
        register_protocol_service = 1 << 9,
        all = ~0u,
    };

    inline PluginPermission operator|(PluginPermission a, PluginPermission b)
    {
        return static_cast<PluginPermission>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    inline PluginPermission operator&(PluginPermission a, PluginPermission b)
    {
        return static_cast<PluginPermission>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    inline bool has_permission(PluginPermission granted, PluginPermission required)
    {
        return (static_cast<uint32_t>(granted) & static_cast<uint32_t>(required)) != 0;
    }

    /// 权限名称表 (用于配置文件和日志)
    struct PluginPermissionNames
    {
        static const char *name(PluginPermission p)
        {
            switch (p) {
            case PluginPermission::use_event_bus:
                return "use_event_bus";
            case PluginPermission::use_logger:
                return "use_logger";
            case PluginPermission::use_scheduler:
                return "use_scheduler";
            case PluginPermission::use_service_catalog:
                return "use_service_catalog";
            case PluginPermission::use_service_registry:
                return "use_service_registry";
            case PluginPermission::use_http_intercept:
                return "use_http_intercept";
            case PluginPermission::use_storage:
                return "use_storage";
            case PluginPermission::use_network_runtime:
                return "use_network_runtime";
            case PluginPermission::use_extension_points:
                return "use_extension_points";
            case PluginPermission::register_protocol_service:
                return "register_protocol_service";
            case PluginPermission::all:
                return "all";
            default:
                return "unknown";
            }
        }

        /// 解析逗号分隔的权限字符串 (如 "use_event_bus,use_logger")
        static PluginPermission parse(const std::string &str);

        /// 将权限位展开为名称列表
        static std::vector<std::string> to_names(PluginPermission p);
    };

} // namespace yuan::plugin

#endif
