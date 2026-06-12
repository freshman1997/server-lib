#ifndef __YUAN_APP_PLUGIN_HOST_SERVICE_H__
#define __YUAN_APP_PLUGIN_HOST_SERVICE_H__

#include "service.h"
#include "plugin/plugin_context.h"
#include "plugin/plugin_events.h"
#include "plugin/host_resource_guard.h"
#include "plugin/plugin_lifecycle_manager.h"
#include "plugin/plugin_permission.h"

#include <memory>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::plugin
{
    class Plugin;
    class PluginManager;
    class HostScheduler;
    class HostServiceRegistry;
    class HostPermissionGuard;
    class HostResourceGuard;
    class HostHttpInterceptor;
    class HostStorage;
    class HostNetworkRuntime;
}

namespace yuan::app
{

    class PluginServiceRegistryAdapter;
    class PluginPermissionGuard;
    class PluginHttpInterceptor;
    class PluginHostScheduler;

    class PluginHostService : public Service, public RuntimeContextAwareService
    {
    public:
        PluginHostService() = default;
        explicit PluginHostService(const std::string &plugin_path);
        PluginHostService(const std::string &plugin_path, const std::vector<std::string> &plugins);
        ~PluginHostService() override;

        void set_plugin_path(const std::string &plugin_path);
        const std::string &plugin_path() const;

        bool add_plugin(const std::string &plugin_name);
        const std::vector<std::string> &plugins() const;

        bool load_plugin(const std::string &plugin_name);
        bool unload_plugin(const std::string &plugin_name);
        plugin::Plugin *get_plugin(const std::string &plugin_name) const;
        plugin::PluginContext plugin_context(const std::string &plugin_name) const;

        std::vector<std::pair<std::string, bool> > health_check_all() const;
        bool health_check(const std::string &plugin_name) const;

        bool reload_config(const std::string &plugin_name);
        bool reload_plugin(const std::string &plugin_name);
        std::vector<std::string> reload_changed_script_plugins();

        void set_default_permissions(plugin::PluginPermission perm);
        void set_plugin_permissions(const std::string &plugin_name, plugin::PluginPermission perm);
        uint64_t track_plugin_resource(const std::string &plugin_name,
                                       plugin::PluginResourceType type,
                                       plugin::ResourceCleanupFn cleanup,
                                       const std::string &description = "");
        bool untrack_plugin_resource(uint64_t resource_id);
        void set_plugin_resource_quota(const std::string &plugin_name, const plugin::PluginResourceQuota &quota);
        std::string resource_leak_report(const std::string &plugin_name) const;

        void set_http_server_accessor(std::function<void *()> accessor);
        void set_http_installers(
            std::function<bool(std::shared_ptr<plugin::HttpMiddlewareCallback>, std::string)> middleware_installer,
            std::function<bool(std::shared_ptr<plugin::HttpRouteCallback>, std::string, std::string, std::string)> route_installer);

        void set_runtime_context(const RuntimeContext &context) override;
        const RuntimeContext &runtime_context() const;

        bool init() override;
        void start() override;
        void stop() override;

        plugin::PluginLifecycleManager &lifecycle_manager();
        const plugin::PluginLifecycleManager &lifecycle_manager() const;

    private:
        plugin::PluginContext make_base_plugin_context(plugin::HostStorage *storage) const;
        plugin::HostStorage *prepare_plugin_storage(const std::string &plugin_name);
        plugin::PluginRunMode to_plugin_run_mode(RunMode mode) const;
        plugin::PluginEvent make_plugin_event(const std::string &plugin_name) const;
        bool script_entry_path(const std::string &plugin_name, std::string &path) const;
        bool update_script_timestamp(const std::string &plugin_name);

        plugin::HostEventBus *event_bus_ptr() const;
        plugin::HostLogger *logger_ptr() const;
        plugin::HostServiceCatalog *service_catalog_ptr() const;
        plugin::HostScheduler *scheduler_ptr() const;
        plugin::HostServiceRegistry *service_registry_ptr() const;
        plugin::HostPermissionGuard *permission_guard_ptr() const;
        plugin::HostResourceGuard *resource_guard_ptr() const;
        plugin::HostHttpInterceptor *http_interceptor_ptr() const;
        plugin::HostNetworkRuntime *network_runtime_ptr() const;
        plugin::PluginManager &plugin_manager() const;

        PluginServiceRegistryAdapter *service_registry_adapter() const;
        PluginPermissionGuard *permission_guard_adapter() const;
        PluginHttpInterceptor *http_interceptor_adapter() const;
        PluginHostScheduler *host_scheduler_adapter() const;

        void setup_lifecycle_callbacks();
        void cleanup_plugin_resources(const std::string &plugin_name);

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
        std::unique_ptr<plugin::HostNetworkRuntime> network_runtime_;
        std::unordered_map<std::string, std::unique_ptr<plugin::HostStorage> > plugin_storages_;
        std::unordered_map<std::string, std::filesystem::file_time_type> script_write_times_;
        std::function<void *()> pending_http_server_accessor_;
        std::function<bool(std::shared_ptr<plugin::HttpMiddlewareCallback>, std::string)> pending_http_middleware_installer_;
        std::function<bool(std::shared_ptr<plugin::HttpRouteCallback>, std::string, std::string, std::string)> pending_http_route_installer_;
        mutable std::unique_ptr<plugin::PluginManager> plugin_manager_;
    };

} // namespace yuan::app

#endif
