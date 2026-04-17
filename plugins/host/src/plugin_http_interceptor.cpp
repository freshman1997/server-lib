#include "plugin_http_interceptor.h"

#include "logger.h"

namespace yuan::app
{

PluginHttpInterceptor::~PluginHttpInterceptor()
{
    remove_by_plugin_internal("");
}

void PluginHttpInterceptor::set_server_accessor(ServerAccessor accessor)
{
    server_accessor_ = std::move(accessor);
}

void PluginHttpInterceptor::set_installers(MiddlewareInstaller middleware_installer,
                                           RouteInstaller route_installer)
{
    middleware_installer_ = std::move(middleware_installer);
    route_installer_ = std::move(route_installer);
    install_pending_entries();
}

void PluginHttpInterceptor::set_resource_guard(plugin::HostResourceGuard *guard)
{
    resource_guard_ = guard;
}

plugin::HttpInterceptorId PluginHttpInterceptor::add_middleware(
    const std::string &plugin_name,
    plugin::HttpMiddlewareCallback callback,
    const std::string &name)
{
    if (!callback || plugin_name.empty()) {
        return 0;
    }

    auto shared_cb = std::make_shared<plugin::HttpMiddlewareCallback>(std::move(callback));
    std::lock_guard<std::mutex> lock(mutex_);
    auto id = next_id_++;

    InterceptorEntry entry;
    entry.id = id;
    entry.plugin_name = plugin_name;
    entry.path = name;
    entry.is_middleware = true;
    entry.shared_callback = shared_cb;
    entry.installed = install_entry(entry);

    if (resource_guard_) {
        auto *self = this;
        entry.resource_guard_id = resource_guard_->track(
            plugin_name,
            plugin::PluginResourceType::http_middleware,
            [self, id]() {
                if (self) {
                    self->remove(id);
                }
            },
            "http-middleware:" + name);
    }

    entries_.emplace(id, std::move(entry));
    plugin_index_[plugin_name].push_back(id);

    LOG_INFO("http interceptor: middleware '{}' registered for plugin '{}' ({})",
             name, plugin_name, entry.installed ? "installed" : "pending");
    return id;
}

plugin::HttpInterceptorId PluginHttpInterceptor::add_route(
    const std::string &plugin_name,
    const std::string &path,
    plugin::HttpRouteCallback callback,
    const std::string &method)
{
    if (!callback || plugin_name.empty() || path.empty()) {
        return 0;
    }

    auto shared_cb = std::make_shared<plugin::HttpRouteCallback>(std::move(callback));
    std::lock_guard<std::mutex> lock(mutex_);
    auto id = next_id_++;

    InterceptorEntry entry;
    entry.id = id;
    entry.plugin_name = plugin_name;
    entry.path = path;
    entry.method = method;
    entry.is_middleware = false;
    entry.shared_route_callback = shared_cb;
    entry.installed = install_entry(entry);

    if (resource_guard_) {
        auto *self = this;
        entry.resource_guard_id = resource_guard_->track(
            plugin_name,
            plugin::PluginResourceType::http_route,
            [self, id]() {
                if (self) {
                    self->remove(id);
                }
            },
            "http-route:" + path);
    }

    entries_.emplace(id, std::move(entry));
    plugin_index_[plugin_name].push_back(id);

    LOG_INFO("http interceptor: route '{}' registered for plugin '{}' ({})",
             path, plugin_name, entry.installed ? "installed" : "pending");
    return id;
}

bool PluginHttpInterceptor::remove(plugin::HttpInterceptorId id)
{
    if (id == 0) {
        return false;
    }

    uint64_t resource_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(id);
        if (it == entries_.end()) {
            return false;
        }

        auto &entry = it->second;
        resource_id = entry.resource_guard_id;

        if (entry.is_middleware && entry.shared_callback) {
            *entry.shared_callback = nullptr;
        }
        if (!entry.is_middleware && entry.shared_route_callback) {
            *entry.shared_route_callback = nullptr;
        }

        auto &ids = plugin_index_[entry.plugin_name];
        for (auto id_it = ids.begin(); id_it != ids.end(); ++id_it) {
            if (*id_it == id) {
                ids.erase(id_it);
                break;
            }
        }
        if (ids.empty()) {
            plugin_index_.erase(entry.plugin_name);
        }

        entries_.erase(it);
    }

    if (resource_guard_ && resource_id != 0) {
        resource_guard_->untrack(resource_id);
    }
    return true;
}

void PluginHttpInterceptor::remove_by_plugin(const std::string &plugin_name)
{
    remove_by_plugin_internal(plugin_name);
}

bool PluginHttpInterceptor::install_entry(InterceptorEntry &entry)
{
    if (entry.installed) {
        return true;
    }

    if (entry.is_middleware) {
        if (!middleware_installer_ || !entry.shared_callback) {
            return false;
        }
        return middleware_installer_(
            entry.shared_callback,
            entry.path.empty() ? entry.plugin_name + ".middleware" : entry.path);
    }

    if (!route_installer_ || !entry.shared_route_callback || entry.path.empty()) {
        return false;
    }

    return route_installer_(
        entry.shared_route_callback,
        entry.path,
        entry.method,
        entry.plugin_name + ":" + entry.path);
}

void PluginHttpInterceptor::install_pending_entries()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &[_, entry] : entries_) {
        if (entry.installed) {
            continue;
        }

        entry.installed = install_entry(entry);
        if (entry.installed) {
            LOG_INFO("http interceptor: pending entry id={} plugin='{}' path='{}' installed",
                     entry.id, entry.plugin_name, entry.path);
        }
    }
}

bool PluginHttpInterceptor::is_available() const
{
    return static_cast<bool>(middleware_installer_) || static_cast<bool>(route_installer_);
}

void PluginHttpInterceptor::remove_by_plugin_internal(const std::string &plugin_name)
{
    std::vector<InterceptorEntry> to_remove;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (plugin_name.empty()) {
            for (auto &[id, entry] : entries_) {
                to_remove.push_back(entry);
            }
            entries_.clear();
            plugin_index_.clear();
        } else {
            auto it = plugin_index_.find(plugin_name);
            if (it == plugin_index_.end()) {
                return;
            }
            for (auto id : it->second) {
                auto entry_it = entries_.find(id);
                if (entry_it != entries_.end()) {
                    to_remove.push_back(entry_it->second);
                    entries_.erase(entry_it);
                }
            }
            plugin_index_.erase(it);
        }
    }

    for (auto &entry : to_remove) {
        if (entry.is_middleware && entry.shared_callback) {
            *entry.shared_callback = nullptr;
        }
        if (!entry.is_middleware && entry.shared_route_callback) {
            *entry.shared_route_callback = nullptr;
        }
        if (resource_guard_ && entry.resource_guard_id != 0) {
            resource_guard_->untrack(entry.resource_guard_id);
        }
        LOG_DEBUG("http interceptor: cleaned up entry id={} plugin='{}' path='{}'",
                  entry.id, entry.plugin_name, entry.path);
    }
}

} // namespace yuan::app
