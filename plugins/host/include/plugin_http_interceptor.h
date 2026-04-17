#ifndef __YUAN_APP_PLUGIN_HTTP_INTERCEPTOR_H__
#define __YUAN_APP_PLUGIN_HTTP_INTERCEPTOR_H__

#include "plugin/host_http_interceptor.h"
#include "plugin/host_resource_guard.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::app
{

class PluginHttpInterceptor : public plugin::HostHttpInterceptor
{
public:
    using ServerAccessor = std::function<void *()>;
    using MiddlewareInstaller = std::function<bool(
        std::shared_ptr<plugin::HttpMiddlewareCallback> callback,
        std::string name)>;
    using RouteInstaller = std::function<bool(
        std::shared_ptr<plugin::HttpRouteCallback> callback,
        std::string path,
        std::string method,
        std::string name)>;

    PluginHttpInterceptor() = default;
    ~PluginHttpInterceptor() override;

    void set_server_accessor(ServerAccessor accessor);
    void set_installers(MiddlewareInstaller middleware_installer,
                        RouteInstaller route_installer);
    void set_resource_guard(plugin::HostResourceGuard *guard);

    plugin::HttpInterceptorId add_middleware(
        const std::string &plugin_name,
        plugin::HttpMiddlewareCallback callback,
        const std::string &name = "") override;

    plugin::HttpInterceptorId add_route(
        const std::string &plugin_name,
        const std::string &path,
        plugin::HttpRouteCallback callback,
        const std::string &method = "") override;

    bool remove(plugin::HttpInterceptorId id) override;
    void remove_by_plugin(const std::string &plugin_name) override;
    bool is_available() const override;

private:
    struct InterceptorEntry
    {
        plugin::HttpInterceptorId id = 0;
        std::string plugin_name;
        std::string path;
        std::string method;
        bool is_middleware = false;
        bool installed = false;
        uint64_t resource_guard_id = 0;
        std::shared_ptr<plugin::HttpMiddlewareCallback> shared_callback;
        std::shared_ptr<plugin::HttpRouteCallback> shared_route_callback;
    };

    void remove_by_plugin_internal(const std::string &plugin_name);
    bool install_entry(InterceptorEntry &entry);
    void install_pending_entries();

    ServerAccessor server_accessor_;
    MiddlewareInstaller middleware_installer_;
    RouteInstaller route_installer_;
    plugin::HostResourceGuard *resource_guard_ = nullptr;

    mutable std::mutex mutex_;
    uint64_t next_id_ = 1;
    std::unordered_map<plugin::HttpInterceptorId, InterceptorEntry> entries_;
    std::unordered_map<std::string, std::vector<plugin::HttpInterceptorId>> plugin_index_;
};

} // namespace yuan::app

#endif
