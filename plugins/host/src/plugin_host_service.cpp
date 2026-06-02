#include "plugin_host_service.h"

#include "base/owner_ptr.h"
#include "eventbus/event_bus.h"
#include "logger.h"
#include "plugin_event_bus_adapter.h"
#include "plugin_host_logger.h"
#include "plugin_host_network_runtime.h"
#include "plugin_host_scheduler.h"
#include "plugin_http_interceptor.h"
#include "plugin_permission_guard.h"
#include "plugin_redis_storage.h"
#include "plugin_resource_guard.h"
#include "plugin_service_catalog.h"
#include "plugin_service_registry_adapter.h"
#include "registry.h"
#include "lua_plugin_module.h"
#include "ts_plugin_module.h"
#include "plugin/plugin_events.h"
#include "plugin/plugin_manager.h"
#include "plugin/plugin_state.h"

#include <algorithm>
#include <utility>
#include <iostream>

namespace yuan::app
{

    plugin::HostEventBus *PluginHostService::event_bus_ptr() const
    {
        return yuan::base::owner_ptr(event_bus_);
    }

    plugin::HostLogger *PluginHostService::logger_ptr() const
    {
        return yuan::base::owner_ptr(logger_);
    }

    plugin::HostServiceCatalog *PluginHostService::service_catalog_ptr() const
    {
        return yuan::base::owner_ptr(service_catalog_);
    }

    plugin::HostScheduler *PluginHostService::scheduler_ptr() const
    {
        return yuan::base::owner_ptr(scheduler_);
    }

    plugin::HostServiceRegistry *PluginHostService::service_registry_ptr() const
    {
        return yuan::base::owner_ptr(service_registry_);
    }

    plugin::HostPermissionGuard *PluginHostService::permission_guard_ptr() const
    {
        return yuan::base::owner_ptr(permission_guard_);
    }

    plugin::HostResourceGuard *PluginHostService::resource_guard_ptr() const
    {
        return yuan::base::owner_ptr(resource_guard_);
    }

    plugin::HostHttpInterceptor *PluginHostService::http_interceptor_ptr() const
    {
        return yuan::base::owner_ptr(http_interceptor_);
    }

    plugin::HostNetworkRuntime *PluginHostService::network_runtime_ptr() const
    {
        return yuan::base::owner_ptr(network_runtime_);
    }

    plugin::PluginManager &PluginHostService::plugin_manager() const
    {
        if (!plugin_manager_) {
            plugin_manager_ = std::make_unique<plugin::PluginManager>();
        }
        return *plugin_manager_;
    }

    PluginServiceRegistryAdapter *PluginHostService::service_registry_adapter() const
    {
        return static_cast<PluginServiceRegistryAdapter *>(service_registry_ptr());
    }

    PluginPermissionGuard *PluginHostService::permission_guard_adapter() const
    {
        return static_cast<PluginPermissionGuard *>(permission_guard_ptr());
    }

    PluginHttpInterceptor *PluginHostService::http_interceptor_adapter() const
    {
        return static_cast<PluginHttpInterceptor *>(http_interceptor_ptr());
    }

    PluginHostScheduler *PluginHostService::host_scheduler_adapter() const
    {
        return static_cast<PluginHostScheduler *>(scheduler_ptr());
    }

    namespace
    {

        plugin::PluginEvent make_plugin_event_impl(const RuntimeContext &context, const std::string &plugin_name)
        {
            plugin::PluginEvent event;
            event.app_name = context.app_name;
            event.plugin_name = plugin_name;
            switch (context.run_mode) {
            case RunMode::single_thread:
                event.run_mode = plugin::PluginRunMode::single_thread;
                break;
            case RunMode::multi_thread:
                event.run_mode = plugin::PluginRunMode::multi_thread;
                break;
            case RunMode::multi_process:
                event.run_mode = plugin::PluginRunMode::multi_process;
                break;
            default:
                event.run_mode = plugin::PluginRunMode::unknown;
                break;
            }
            event.worker_threads = context.worker_threads;
            event.runtime_worker_count = context.runtime_worker_count == 0
                ? context.worker_threads
                : context.runtime_worker_count;
            event.worker_index = context.worker_index;
            event.is_worker_process = context.is_worker_process;
            event.active_service_name = context.active_service_name;
            event.service_index = context.service_index;
            event.service_instance_index = context.service_instance_index;
            event.service_instance_count = context.service_instance_count == 0
                ? 1
                : context.service_instance_count;
            event.listener_reuse_port = context.listener_reuse_port;
            return event;
        }

        plugin::PluginLoadFailedEvent make_plugin_load_failed_event(
            const RuntimeContext &context,
            const std::string &plugin_name,
            std::string reason)
        {
            plugin::PluginLoadFailedEvent event;
            static_cast<plugin::PluginEvent &>(event) = make_plugin_event_impl(context, plugin_name);
            event.reason = std::move(reason);
            return event;
        }

    } // namespace

    plugin::PluginContext PluginHostService::make_base_plugin_context(plugin::HostStorage * storage) const
    {
        plugin::PluginContext context;
        context.app_name = runtime_context_.app_name;
        context.plugin_root_path = plugin_path_;
        context.run_mode = to_plugin_run_mode(runtime_context_.run_mode);
        context.worker_threads = runtime_context_.worker_threads;
        context.runtime_worker_count = runtime_context_.runtime_worker_count == 0
            ? runtime_context_.worker_threads
            : runtime_context_.runtime_worker_count;
        context.worker_index = runtime_context_.worker_index;
        context.is_worker_process = runtime_context_.is_worker_process;
        context.active_service_name = runtime_context_.active_service_name;
        context.service_index = runtime_context_.service_index;
        context.service_instance_index = runtime_context_.service_instance_index;
        context.service_instance_count = runtime_context_.service_instance_count == 0
            ? 1
            : runtime_context_.service_instance_count;
        context.listener_reuse_port = runtime_context_.listener_reuse_port;
        context.event_bus = event_bus_ptr();
        context.logger = logger_ptr();
        context.service_catalog = service_catalog_ptr();
        context.scheduler = scheduler_ptr();
        context.service_registry = service_registry_ptr();
        context.permission_guard = permission_guard_ptr();
        context.resource_guard = resource_guard_ptr();
        context.http_interceptor = http_interceptor_ptr();
        context.storage = storage;
        context.network_runtime = network_runtime_ptr();
        context.extension_point_registry = nullptr;
        context.granted_permissions = plugin::PluginPermission::none;
        return context;
    }

    plugin::HostStorage *PluginHostService::prepare_plugin_storage(const std::string & plugin_name)
    {
        auto it = plugin_storages_.find(plugin_name);
        if (it != plugin_storages_.end()) {
            return yuan::base::owner_ptr(it->second);
        }

        auto storage = std::make_unique<PluginRedisStorage>(plugin_name);
        auto *raw = yuan::base::owner_ptr(storage);
        if (!storage->init()) {
            LOG_WARN("plugin storage for '{}' is present but backend is unavailable", plugin_name);
        }

        plugin_storages_.emplace(plugin_name, std::move(storage));
        return raw;
    }

    PluginHostService::PluginHostService(const std::string & plugin_path)
        : plugin_path_(plugin_path)
    {
    }

    PluginHostService::PluginHostService(const std::string & plugin_path, const std::vector<std::string> & plugins)
        : plugin_path_(plugin_path), plugin_names_(plugins)
    {
    }

    PluginHostService::~PluginHostService()
    {
        stop();
    }

    void PluginHostService::set_plugin_path(const std::string & plugin_path)
    {
        plugin_path_ = plugin_path;
    }

    const std::string &PluginHostService::plugin_path() const
    {
        return plugin_path_;
    }

    bool PluginHostService::add_plugin(const std::string & plugin_name)
    {
        if (plugin_name.empty()) {
            return false;
        }

        const auto it = std::find(plugin_names_.begin(), plugin_names_.end(), plugin_name);
        if (it != plugin_names_.end()) {
            return false;
        }

        plugin_names_.push_back(plugin_name);
        return true;
    }

    const std::vector<std::string> &PluginHostService::plugins() const
    {
        return plugin_names_;
    }

    void PluginHostService::setup_lifecycle_callbacks()
    {
        auto &pluginManager = plugin_manager();
        auto &lcm = pluginManager.lifecycle_manager();

        lcm.call_guard().set_fault_handler([this](const plugin::FaultEvent &event) {
        LOG_ERROR("plugin '{}' fault in '{}': {}",
                  event.plugin_name, event.call_site, event.error_message);

        if (runtime_context_.event_bus) {
            plugin::PluginFaultEvent fault_event;
            static_cast<plugin::PluginEvent &>(fault_event) = make_plugin_event(event.plugin_name);
            fault_event.fault_message = event.error_message;
            fault_event.call_site = event.call_site;
            runtime_context_.event_bus->publish(plugin::events::plugin_faulted, fault_event);
        }
        });

        lcm.set_state_change_callback([this](const std::string &plugin_name,
                                             plugin::PluginState old_state,
                                             plugin::PluginState new_state) {
        LOG_INFO("plugin '{}' lifecycle: {} -> {}",
                 plugin_name, plugin::to_string(old_state), plugin::to_string(new_state));

        if (runtime_context_.event_bus) {
            if (new_state == plugin::PluginState::initialized) {
                runtime_context_.event_bus->publish(
                    plugin::events::plugin_initialized, make_plugin_event(plugin_name));
            } else if (new_state == plugin::PluginState::active) {
                runtime_context_.event_bus->publish(
                    plugin::events::plugin_activated, make_plugin_event(plugin_name));
            } else if (new_state == plugin::PluginState::degraded) {
                runtime_context_.event_bus->publish(
                    plugin::events::plugin_degraded, make_plugin_event(plugin_name));
            } else if (new_state == plugin::PluginState::quarantined) {
                runtime_context_.event_bus->publish(
                    plugin::events::plugin_quarantined, make_plugin_event(plugin_name));
            }
        }
        });
    }

    void PluginHostService::cleanup_plugin_resources(const std::string & plugin_name)
    {
        if (http_interceptor_) {
            http_interceptor_->remove_by_plugin(plugin_name);
        }

        if (resource_guard_) {
            resource_guard_->cleanup_plugin(plugin_name);
        }

        if (service_registry_) {
            service_registry_->unregister_plugin_services(plugin_name);
        }

        if (permission_guard_) {
            permission_guard_->revoke(plugin_name, plugin::PluginPermission::all);
        }
    }

    bool PluginHostService::load_plugin(const std::string & plugin_name)
    {
        auto &pluginManager = plugin_manager();

        auto *plugin_storage = prepare_plugin_storage(plugin_name);
        pluginManager.set_context(make_base_plugin_context(plugin_storage));

        if (!pluginManager.load(plugin_name)) {
            LOG_ERROR("runtime load plugin '{}' failed", plugin_name);
            if (runtime_context_.event_bus) {
                runtime_context_.event_bus->publish(
                    plugin::events::plugin_load_failed,
                    make_plugin_load_failed_event(runtime_context_, plugin_name, "runtime load failed"));
            }
            plugin_storages_.erase(plugin_name);
            pluginManager.set_context(make_base_plugin_context(nullptr));
            return false;
        }

        if (service_registry_) {
            auto *registry = service_registry_adapter();
            if (!registry->init_plugin_services(plugin_name, pluginManager.plugin_context(plugin_name))) {
                cleanup_plugin_resources(plugin_name);
                pluginManager.release_plugin(plugin_name);
                plugin_storages_.erase(plugin_name);
                pluginManager.set_context(make_base_plugin_context(nullptr));
                return false;
            }
        }

        auto &lcm = pluginManager.lifecycle_manager();
        if (!lcm.activate(plugin_name)) {
            LOG_ERROR("failed to activate plugin '{}'", plugin_name);
            cleanup_plugin_resources(plugin_name);
            pluginManager.release_plugin(plugin_name);
            plugin_storages_.erase(plugin_name);
            pluginManager.set_context(make_base_plugin_context(nullptr));
            return false;
        }

        auto *plugin = pluginManager.get_plugin(plugin_name);
        if (plugin) {
            if (!lcm.call_guard().guarded_call_void(plugin_name, lcm.state(plugin_name), "on_enable",
                                                    [plugin]() { plugin->on_enable(); })) {
                LOG_ERROR("plugin '{}' on_enable failed, rolling back", plugin_name);
                cleanup_plugin_resources(plugin_name);
                pluginManager.release_plugin(plugin_name);
                plugin_storages_.erase(plugin_name);
                pluginManager.set_context(make_base_plugin_context(nullptr));
                return false;
            }
        }

        if (started_ && service_registry_ && lcm.state(plugin_name) == plugin::PluginState::active) {
            if (!service_registry_adapter()->start_plugin_services(plugin_name)) {
                LOG_ERROR("plugin '{}' managed services failed to start, rolling back", plugin_name);
                cleanup_plugin_resources(plugin_name);
                pluginManager.release_plugin(plugin_name);
                plugin_storages_.erase(plugin_name);
                pluginManager.set_context(make_base_plugin_context(nullptr));
                return false;
            }
        }

        loaded_plugins_.push_back(plugin_name);
        if (runtime_context_.event_bus) {
            runtime_context_.event_bus->publish(
                plugin::events::plugin_loaded,
                make_plugin_event(plugin_name));
        }
        return true;
    }

    bool PluginHostService::unload_plugin(const std::string & plugin_name)
    {
        auto it = std::find(loaded_plugins_.begin(), loaded_plugins_.end(), plugin_name);
        if (it == loaded_plugins_.end()) {
            return false;
        }

        auto &pluginManager = plugin_manager();
        auto &lcm = pluginManager.lifecycle_manager();

        if (runtime_context_.event_bus) {
            runtime_context_.event_bus->publish(
                plugin::events::plugin_unloading,
                make_plugin_event(plugin_name));
        }

        auto *plugin = pluginManager.get_plugin(plugin_name);
        if (plugin) {
            lcm.call_guard().guarded_call_void(plugin_name, lcm.state(plugin_name), "on_disable",
                                               [plugin]() { plugin->on_disable(); });
        }

        if (service_registry_) {
            service_registry_adapter()->stop_plugin_services(plugin_name);
        }

        lcm.stop(plugin_name);

        pluginManager.release_plugin(plugin_name);
        plugin_storages_.erase(plugin_name);
        pluginManager.set_context(make_base_plugin_context(nullptr));
        loaded_plugins_.erase(it);

        if (runtime_context_.event_bus) {
            runtime_context_.event_bus->publish(
                plugin::events::plugin_unloaded,
                make_plugin_event(plugin_name));
        }
        return true;
    }

    plugin::Plugin *PluginHostService::get_plugin(const std::string & plugin_name) const
    {
        return plugin_manager().get_plugin(plugin_name);
    }

    plugin::PluginContext PluginHostService::plugin_context(const std::string &plugin_name) const
    {
        return plugin_manager().plugin_context(plugin_name);
    }

    std::vector<std::pair<std::string, bool> > PluginHostService::health_check_all() const
    {
        std::vector<std::pair<std::string, bool> > results;
        auto &pluginManager = plugin_manager();
        auto &lcm = pluginManager.lifecycle_manager();

        for (const auto &name : loaded_plugins_) {
            auto *plugin = pluginManager.get_plugin(name);
            bool healthy = false;

            if (plugin && lcm.accepts_callbacks(name)) {
                healthy = lcm.call_guard().guarded_call_void(
                    name, lcm.state(name), "on_health_check",
                    [plugin]()->bool { return plugin->on_health_check(); });
            }

            results.emplace_back(name, healthy);

            if (runtime_context_.event_bus) {
                plugin::PluginHealthCheckEvent event;
                static_cast<plugin::PluginEvent &>(event) = make_plugin_event(name);
                event.healthy = healthy;
                runtime_context_.event_bus->publish(plugin::events::plugin_health_checked, event);
            }

            if (!healthy && lcm.state(name) == plugin::PluginState::active) {
                lcm.fault(name, "health check failed");
            }
        }
        return results;
    }

    bool PluginHostService::health_check(const std::string & plugin_name) const
    {
        auto &pluginManager = plugin_manager();
        auto &lcm = pluginManager.lifecycle_manager();
        auto *plugin = pluginManager.get_plugin(plugin_name);
        if (!plugin) {
            return false;
        }

        bool healthy = false;
        if (lcm.accepts_callbacks(plugin_name)) {
            healthy = lcm.call_guard().guarded_call_void(
                plugin_name, lcm.state(plugin_name), "on_health_check",
                [plugin]()->bool { return plugin->on_health_check(); });
        }

        if (runtime_context_.event_bus) {
            plugin::PluginHealthCheckEvent event;
            static_cast<plugin::PluginEvent &>(event) = make_plugin_event(plugin_name);
            event.healthy = healthy;
            runtime_context_.event_bus->publish(plugin::events::plugin_health_checked, event);
        }

        if (!healthy && lcm.state(plugin_name) == plugin::PluginState::active) {
            lcm.fault(plugin_name, "health check failed");
        }

        return healthy;
    }

    bool PluginHostService::reload_config(const std::string & plugin_name)
    {
        auto it = std::find(loaded_plugins_.begin(), loaded_plugins_.end(), plugin_name);
        if (it == loaded_plugins_.end()) {
            return false;
        }

        auto &pluginManager = plugin_manager();
        auto &lcm = pluginManager.lifecycle_manager();
        auto *plugin = pluginManager.get_plugin(plugin_name);
        if (!plugin) {
            return false;
        }
        auto plugin_context = pluginManager.plugin_context(plugin_name);

        auto new_config = pluginManager.reload_plugin_config(plugin_name);
        if (!new_config.loaded()) {
            LOG_WARN("reload config for plugin '{}' failed: config not loaded", plugin_name);
            return false;
        }

        if (lcm.accepts_callbacks(plugin_name)) {
            lcm.call_guard().guarded_call_void(
                plugin_name, lcm.state(plugin_name), "on_config_changed",
                [plugin, &new_config]() { plugin->on_config_changed(new_config); });
        }

        if (runtime_context_.event_bus) {
            plugin::PluginConfigChangedEvent event;
            static_cast<plugin::PluginEvent &>(event) = make_plugin_event(plugin_name);
            event.config_path = plugin_context.plugin_config_path;
            runtime_context_.event_bus->publish(plugin::events::plugin_config_changed, event);
        }
        return true;
    }

    void PluginHostService::set_default_permissions(plugin::PluginPermission perm)
    {
        if (permission_guard_) {
            permission_guard_adapter()->set_default_permissions(perm);
        }
    }

    void PluginHostService::set_plugin_permissions(const std::string & plugin_name, plugin::PluginPermission perm)
    {
        if (permission_guard_) {
            permission_guard_->revoke(plugin_name, plugin::PluginPermission::all);
            permission_guard_->grant(plugin_name, perm);
        }
    }

    uint64_t PluginHostService::track_plugin_resource(const std::string &plugin_name,
                                                      plugin::PluginResourceType type,
                                                      plugin::ResourceCleanupFn cleanup,
                                                      const std::string &description)
    {
        if (!resource_guard_) {
            return 0;
        }
        return resource_guard_->track(plugin_name, type, std::move(cleanup), description);
    }

    bool PluginHostService::untrack_plugin_resource(uint64_t resource_id)
    {
        if (!resource_guard_) {
            return false;
        }
        return resource_guard_->untrack(resource_id);
    }

    std::string PluginHostService::resource_leak_report(const std::string &plugin_name) const
    {
        if (!resource_guard_) {
            return {};
        }
        return resource_guard_->leak_report(plugin_name);
    }

    void PluginHostService::set_http_server_accessor(std::function<void *()> accessor)
    {
        pending_http_server_accessor_ = std::move(accessor);
        if (http_interceptor_) {
            http_interceptor_adapter()->set_server_accessor(
                [acc = pending_http_server_accessor_]()->void * {
                return acc ? acc() : nullptr;
                });
        }
    }

    void PluginHostService::set_http_installers(
        std::function<bool(std::shared_ptr<plugin::HttpMiddlewareCallback>, std::string)> middleware_installer,
        std::function<bool(std::shared_ptr<plugin::HttpRouteCallback>, std::string, std::string, std::string)> route_installer)
    {
        pending_http_middleware_installer_ = std::move(middleware_installer);
        pending_http_route_installer_ = std::move(route_installer);
        if (http_interceptor_) {
            http_interceptor_adapter()->set_installers(
                pending_http_middleware_installer_,
                pending_http_route_installer_);
        }
    }

    void PluginHostService::set_runtime_context(const RuntimeContext & context)
    {
        runtime_context_ = context;
    }

    const RuntimeContext &PluginHostService::runtime_context() const
    {
        return runtime_context_;
    }

    plugin::PluginRunMode PluginHostService::to_plugin_run_mode(RunMode mode) const
    {
        switch (mode) {
        case RunMode::single_thread:
            return plugin::PluginRunMode::single_thread;
        case RunMode::multi_thread:
            return plugin::PluginRunMode::multi_thread;
        case RunMode::multi_process:
            return plugin::PluginRunMode::multi_process;
        default:
            return plugin::PluginRunMode::unknown;
        }
    }

    plugin::PluginEvent PluginHostService::make_plugin_event(const std::string & plugin_name) const
    {
        return make_plugin_event_impl(runtime_context_, plugin_name);
    }

    plugin::PluginLifecycleManager &PluginHostService::lifecycle_manager()
    {
        return plugin_manager().lifecycle_manager();
    }

    const plugin::PluginLifecycleManager &PluginHostService::lifecycle_manager() const
    {
        return plugin_manager().lifecycle_manager();
    }

    bool PluginHostService::init()
    {
        plugin::init_lua_plugin_module();
        plugin::init_ts_plugin_module();

        service_catalog_.reset();
        if (runtime_context_.service_registry) {
            service_catalog_ = std::make_unique<PluginServiceCatalog>(runtime_context_.service_registry);
        }
        event_bus_.reset();
        if (runtime_context_.event_bus) {
            event_bus_ = std::make_unique<PluginEventBusAdapter>(runtime_context_.event_bus);
        }
        logger_ = std::make_unique<PluginHostLogger>();
        scheduler_ = std::make_unique<PluginHostScheduler>();
        service_registry_ = std::make_unique<PluginServiceRegistryAdapter>();
        permission_guard_ = std::make_unique<PluginPermissionGuard>();
        resource_guard_ = std::make_unique<PluginResourceGuard>();
        http_interceptor_ = std::make_unique<PluginHttpInterceptor>();
        if (runtime_context_.shared_runtime) {
            network_runtime_ = std::make_unique<PluginHostNetworkRuntime>(runtime_context_.shared_runtime);
        }

        http_interceptor_adapter()->set_resource_guard(resource_guard_ptr());
        if (pending_http_server_accessor_) {
            http_interceptor_adapter()->set_server_accessor(
                [acc = pending_http_server_accessor_]()->void * {
                return acc ? acc() : nullptr;
                });
        }
        if (pending_http_middleware_installer_ || pending_http_route_installer_) {
            http_interceptor_adapter()->set_installers(
                pending_http_middleware_installer_,
                pending_http_route_installer_);
        }

        auto &pluginManager = plugin_manager();
        pluginManager.set_plugin_path(plugin_path_);

        setup_lifecycle_callbacks();

        loaded_plugins_.clear();
        for (const auto &pluginName : plugin_names_) {
            auto *plugin_storage = prepare_plugin_storage(pluginName);
            pluginManager.set_context(make_base_plugin_context(plugin_storage));

            if (!pluginManager.load(pluginName)) {
                LOG_ERROR("plugin host failed to load plugin '{}'", pluginName);
                if (runtime_context_.event_bus) {
                    runtime_context_.event_bus->publish(
                        plugin::events::plugin_load_failed,
                        make_plugin_load_failed_event(runtime_context_, pluginName, "plugin manager load returned false"));
                }
                plugin_storages_.erase(pluginName);
                for (auto it = loaded_plugins_.rbegin(); it != loaded_plugins_.rend(); ++it) {
                    cleanup_plugin_resources(*it);
                    pluginManager.release_plugin(*it);
                    plugin_storages_.erase(*it);
                }
                loaded_plugins_.clear();
                pluginManager.set_context(make_base_plugin_context(nullptr));
                return false;
            }

            pluginManager.set_plugin_storage(pluginName, plugin_storage);

            if (service_registry_) {
                auto *registry = service_registry_adapter();
                if (!registry->init_plugin_services(pluginName, pluginManager.plugin_context(pluginName))) {
                    LOG_ERROR("plugin host failed to init services for plugin '{}'", pluginName);
                    cleanup_plugin_resources(pluginName);
                    pluginManager.release_plugin(pluginName);
                    plugin_storages_.erase(pluginName);
                    for (auto rit = loaded_plugins_.rbegin(); rit != loaded_plugins_.rend(); ++rit) {
                        cleanup_plugin_resources(*rit);
                        pluginManager.release_plugin(*rit);
                        plugin_storages_.erase(*rit);
                    }
                    loaded_plugins_.clear();
                    pluginManager.set_context(make_base_plugin_context(nullptr));
                    return false;
                }
            }

            loaded_plugins_.push_back(pluginName);
            if (runtime_context_.event_bus) {
                runtime_context_.event_bus->publish(
                    plugin::events::plugin_loaded,
                    make_plugin_event(pluginName));
            }
        }

        return true;
    }

    void PluginHostService::start()
    {
        if (started_) {
            return;
        }

        auto &pluginManager = plugin_manager();
        auto &lcm = pluginManager.lifecycle_manager();

        for (const auto &plugin_name : loaded_plugins_) {
            if (!lcm.activate(plugin_name)) {
                LOG_WARN("plugin '{}' cannot be activated during start, skipping on_enable", plugin_name);
                continue;
            }

            auto *plugin = pluginManager.get_plugin(plugin_name);
            if (plugin) {
                if (!lcm.call_guard().guarded_call_void(
                    plugin_name, lcm.state(plugin_name), "on_enable",
                    [plugin]() { plugin->on_enable(); })) {
                    LOG_ERROR("plugin '{}' on_enable failed during start", plugin_name);
                    lcm.fault(plugin_name, "on_enable failed during start");
                }
            }

            if (service_registry_ && lcm.state(plugin_name) == plugin::PluginState::active) {
                if (!service_registry_adapter()->start_plugin_services(plugin_name)) {
                    LOG_ERROR("plugin '{}' managed services failed to start during host start", plugin_name);
                    lcm.fault(plugin_name, "managed services failed to start");
                }
            }
        }

        started_ = true;
    }

    void PluginHostService::stop()
    {
        auto &pluginManager = plugin_manager();
        auto &lcm = pluginManager.lifecycle_manager();

        for (auto it = loaded_plugins_.rbegin(); it != loaded_plugins_.rend(); ++it) {
            auto *plugin = pluginManager.get_plugin(*it);
            if (plugin) {
                lcm.call_guard().guarded_call_void(
                    *it, lcm.state(*it), "on_disable",
                    [plugin]() { plugin->on_disable(); });
            }
        }

        if (service_registry_) {
            auto *registry = service_registry_adapter();
            for (auto it = loaded_plugins_.rbegin(); it != loaded_plugins_.rend(); ++it) {
                registry->stop_plugin_services(*it);
            }
        }
        started_ = false;

        if (scheduler_) {
            host_scheduler_adapter()->shutdown();
        }

        for (auto it = loaded_plugins_.rbegin(); it != loaded_plugins_.rend(); ++it) {
            if (runtime_context_.event_bus) {
                runtime_context_.event_bus->publish(
                    plugin::events::plugin_unloading,
                    make_plugin_event(*it));
            }

            lcm.stop(*it);

            pluginManager.release_plugin(*it);
            plugin_storages_.erase(*it);
            if (runtime_context_.event_bus) {
                runtime_context_.event_bus->publish(
                    plugin::events::plugin_unloaded,
                    make_plugin_event(*it));
            }
        }
        loaded_plugins_.clear();
        pluginManager.set_context(make_base_plugin_context(nullptr));

        resource_guard_.reset();
        scheduler_.reset();
        http_interceptor_.reset();
        network_runtime_.reset();
        plugin_storages_.clear();
        service_registry_.reset();
        permission_guard_.reset();
        logger_.reset();
        event_bus_.reset();
        service_catalog_.reset();
    }

} // namespace yuan::app
