#ifndef __YUAN_PLUGIN_HOST_PERMISSION_GUARD_H__
#define __YUAN_PLUGIN_HOST_PERMISSION_GUARD_H__

#include "plugin/plugin_permission.h"

namespace yuan::plugin
{

/// 权限守卫 — 注入到 PluginContext 中, 各宿主接口在关键操作前调用 check()
class HostPermissionGuard
{
public:
    virtual ~HostPermissionGuard() = default;

    /// 检查指定插件是否拥有指定权限; 无权限时返回 false
    virtual bool check(const std::string &plugin_name, PluginPermission perm) const = 0;

    /// 授予指定插件权限 (合并, 不覆盖已有)
    virtual void grant(const std::string &plugin_name, PluginPermission perm) = 0;

    /// 撤销指定插件的指定权限
    virtual void revoke(const std::string &plugin_name, PluginPermission perm) = 0;

    /// 获取指定插件当前权限
    virtual PluginPermission get_permissions(const std::string &plugin_name) const = 0;
};

} // namespace yuan::plugin

#endif
