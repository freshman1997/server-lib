#ifndef __YUAN_APP_PLUGIN_PERMISSION_GUARD_H__
#define __YUAN_APP_PLUGIN_PERMISSION_GUARD_H__

#include "plugin/host_permission_guard.h"

#include <mutex>
#include <unordered_map>

namespace yuan::app
{

/// 基于 mutex 保护的权限守卫实现
class PluginPermissionGuard : public plugin::HostPermissionGuard
{
public:
    PluginPermissionGuard() = default;

    bool check(const std::string &plugin_name, plugin::PluginPermission perm) const override;
    void grant(const std::string &plugin_name, plugin::PluginPermission perm) override;
    void revoke(const std::string &plugin_name, plugin::PluginPermission perm) override;
    plugin::PluginPermission get_permissions(const std::string &plugin_name) const override;

    /// 设置默认权限 — 未显式配置的插件将获得此权限
    void set_default_permissions(plugin::PluginPermission perm);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, plugin::PluginPermission> permissions_;
    plugin::PluginPermission default_permissions_ = plugin::PluginPermission::all;
};

} // namespace yuan::app

#endif
