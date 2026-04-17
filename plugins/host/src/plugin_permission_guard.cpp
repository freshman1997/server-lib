#include "plugin_permission_guard.h"
#include "logger.h"

namespace yuan::app
{

bool PluginPermissionGuard::check(const std::string &plugin_name, plugin::PluginPermission perm) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = permissions_.find(plugin_name);
    plugin::PluginPermission granted = (it != permissions_.end()) ? it->second : default_permissions_;
    return has_permission(granted, perm);
}

void PluginPermissionGuard::grant(const std::string &plugin_name, plugin::PluginPermission perm)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto &current = permissions_[plugin_name];
    current = current | perm;
}

void PluginPermissionGuard::revoke(const std::string &plugin_name, plugin::PluginPermission perm)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = permissions_.find(plugin_name);
    if (it != permissions_.end()) {
        it->second = static_cast<plugin::PluginPermission>(
            static_cast<uint32_t>(it->second) & ~static_cast<uint32_t>(perm));
    }
}

plugin::PluginPermission PluginPermissionGuard::get_permissions(const std::string &plugin_name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = permissions_.find(plugin_name);
    return (it != permissions_.end()) ? it->second : default_permissions_;
}

void PluginPermissionGuard::set_default_permissions(plugin::PluginPermission perm)
{
    std::lock_guard<std::mutex> lock(mutex_);
    default_permissions_ = perm;
}

} // namespace yuan::app
