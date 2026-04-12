#ifndef __YUAN_APP_PLUGIN_HOST_SERVICE_H__
#define __YUAN_APP_PLUGIN_HOST_SERVICE_H__

#include "service.h"
#include "plugin/plugin_context.h"
#include "plugin/plugin_events.h"
#include "plugin/plugin_permission.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::plugin
{
class HostScheduler;
class HostServiceRegistry;
class HostPermissionGuard;
class HostResourceGuard;
class HostHttpInterceptor;
class HostStorage;
}

namespace yuan::app
{

class PluginHostService : public Service, public RuntimeContextAwareService
{
public:
    PluginHostService() = default;
    explicit PluginHostService(std::string plugin_path);
    PluginHostService(std::string plugin_path, std::vector<std::string> plugins);

    void set_plugin_path(std::string plugin_path);
    const std::string& plugin_path() const;

    bool add_plugin(std::string plugin_name);
    const std::vector<std::string>& plugins() const;

    /// 运行时动态加载单个插件 (可在 init 之后调用)
    bool load_plugin(const std::string &plugin_name);

    /// 运行时卸载单个插件
    bool unload_plugin(const std::string &plugin_name);

    /// 对所有插件执行健康检查, 返回 {plugin_name -> healthy}
    std::vector<std::pair<std::string, bool>> health_check_all();

    /// 对单个插件执行健康检查
    bool health_check(const std::string &plugin_name);

    /// 触发配置热更新 (重新加载指定插件的 JSON 配置并通知插件)
    bool reload_config(const std::string &plugin_name);

    /// 设置默认权限 (未显式配置的插件获得的权限)
    void set_default_permissions(plugin::PluginPermission perm);

    /// 为指定插件设置权限 (覆盖 meta 中的 required_permissions)
    void set_plugin_permissions(const std::string &plugin_name, plugin::PluginPermission perm);

    /// 设置 HTTP Server 访问器 (由 server 层调用, 桥接 HttpServer 到插件系统)
    void set_http_server_accessor(std::function<void *()> accessor);
    void set_http_installers(
        std::function<bool(std::shared_ptr<plugin::HttpMiddlewareCallback>, std::string)> middleware_installer,
        std::function<bool(std::shared_ptr<plugin::HttpRouteCallback>, std::string, std::string, std::string)> route_installer);

    void set_runtime_context(const RuntimeContext &context) override;

    bool init() override;
    void start() override;
    void stop() override;

private:
    plugin::PluginContext make_base_plugin_context(plugin::HostStorage *storage) const;
    plugin::HostStorage *prepare_plugin_storage(const std::string &plugin_name);
    plugin::PluginRunMode to_plugin_run_mode(RunMode mode) const;
    plugin::PluginEvent make_plugin_event(const std::string &plugin_name) const;

    std::string plugin_path_;
    std::vector<std::string> plugin_names_;
    std::vector<std::string> loaded_plugins_;
    bool started_ = false;
    RuntimeContext runtime_context_{};
    std::unique_ptr<plugin::HostEventBus> event_bus_;
    std::unique_ptr<plugin::HostLogger> logger_;
    std::unique_ptr<plugin::HostServiceCatalog> service_catalog_;
    std::unique_ptr<plugin::HostScheduler> scheduler_;
    std::unique_ptr<plugin::HostServiceRegistry> service_registry_;
    std::unique_ptr<plugin::HostPermissionGuard> permission_guard_;
    std::unique_ptr<plugin::HostResourceGuard> resource_guard_;
    std::unique_ptr<plugin::HostHttpInterceptor> http_interceptor_;
    std::unordered_map<std::string, std::unique_ptr<plugin::HostStorage>> plugin_storages_;
    std::function<void *()> pending_http_server_accessor_;
    std::function<bool(std::shared_ptr<plugin::HttpMiddlewareCallback>, std::string)> pending_http_middleware_installer_;
    std::function<bool(std::shared_ptr<plugin::HttpRouteCallback>, std::string, std::string, std::string)> pending_http_route_installer_;
};

} // namespace yuan::app

#endif
