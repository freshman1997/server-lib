#ifndef __PLUGIN_CONTEXT_H__
#define __PLUGIN_CONTEXT_H__

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "plugin/host_event_bus.h"
#include "plugin/host_http_interceptor.h"
#include "plugin/host_logger.h"
#include "plugin/host_network_runtime.h"
#include "plugin/host_permission_guard.h"
#include "plugin/host_resource_guard.h"
#include "plugin/host_scheduler.h"
#include "plugin/host_service_catalog.h"
#include "plugin/host_service_registry.h"
#include "plugin/host_storage.h"
#include "plugin/extension_point_registry.h"
#include "plugin/plugin_config_view.h"
#include "plugin/plugin_manifest.h"
#include "plugin/plugin_permission.h"

namespace yuan::plugin
{

    struct PluginSdkBoundary
    {
        int host_api_version = 1;
        std::string app_name;
        std::string plugin_name;
        std::string plugin_root_path;
        std::string plugin_config_path;
        PluginRunMode run_mode = PluginRunMode::unknown;
        std::size_t worker_threads = 1;
        std::size_t runtime_worker_count = 1;
        std::size_t worker_index = 0;
        bool is_worker_process = false;
        std::string active_service_name;
        std::size_t service_index = 0;
        std::size_t service_instance_index = 0;
        std::size_t service_instance_count = 1;
        bool listener_reuse_port = false;
    };

    struct PluginCapabilitySnapshot
    {
        PluginPermission granted_permissions = PluginPermission::none;
        bool event_bus = false;
        bool logger = false;
        bool service_catalog = false;
        bool scheduler = false;
        bool service_registry = false;
        bool permission_guard = false;
        bool resource_guard = false;
        bool http_interceptor = false;
        bool storage = false;
        bool network_runtime = false;
        bool extension_points = false;
    };

    struct PluginContext
    {
        std::string app_name;
        std::string plugin_name;
        std::string plugin_root_path;
        std::string plugin_config_path;
        PluginConfigView config;
        PluginRunMode run_mode = PluginRunMode::unknown;
        std::size_t worker_threads = 1;
        std::size_t runtime_worker_count = 1;
        std::size_t worker_index = 0;
        bool is_worker_process = false;
        std::string active_service_name;
        std::size_t service_index = 0;
        std::size_t service_instance_index = 0;
        std::size_t service_instance_count = 1;
        bool listener_reuse_port = false;

        HostEventBus *event_bus = nullptr;
        HostLogger *logger = nullptr;
        HostServiceCatalog *service_catalog = nullptr;
        HostScheduler *scheduler = nullptr;
        HostServiceRegistry *service_registry = nullptr;
        HostPermissionGuard *permission_guard = nullptr;
        HostResourceGuard *resource_guard = nullptr;
        HostHttpInterceptor *http_interceptor = nullptr;
        HostStorage *storage = nullptr;
        HostNetworkRuntime *network_runtime = nullptr;
        const ExtensionPointRegistry *extension_point_registry = nullptr;
        PluginPermission granted_permissions = PluginPermission::none;

        PluginSdkBoundary sdk_boundary() const
        {
            return PluginSdkBoundary{
                1,
                app_name,
                plugin_name,
                plugin_root_path,
                plugin_config_path,
                run_mode,
                worker_threads,
                runtime_worker_count,
                worker_index,
                is_worker_process,
                active_service_name,
                service_index,
                service_instance_index,
                service_instance_count,
                listener_reuse_port
            };
        }

        PluginCapabilitySnapshot capabilities() const
        {
            return PluginCapabilitySnapshot{
                granted_permissions,
                event_bus != nullptr,
                logger != nullptr,
                service_catalog != nullptr,
                scheduler != nullptr,
                service_registry != nullptr,
                permission_guard != nullptr,
                resource_guard != nullptr,
                http_interceptor != nullptr,
                storage != nullptr,
                network_runtime != nullptr,
                extension_point_registry != nullptr
            };
        }

        bool can_use(PluginPermission permission) const
        {
            return has_permission(granted_permissions, permission);
        }

        bool has_capability(PluginPermission permission, const void *capability) const
        {
            return capability != nullptr && can_use(permission);
        }
    };

    class PluginContextHelper
    {
    public:
        explicit PluginContextHelper(const PluginContext &ctx)
            : ctx_(ctx)
        {
        }

        uint64_t track_resource(PluginResourceType type, ResourceCleanupFn cleanup,
                                const std::string &description = "") const
        {
            if (ctx_.resource_guard) {
                return ctx_.resource_guard->track(ctx_.plugin_name, type, std::move(cleanup), description);
            }
            return 0;
        }

        uint64_t track_callback(ResourceCleanupFn cleanup, const std::string &description = "") const
        {
            return track_resource(PluginResourceType::callback, std::move(cleanup), description);
        }

        uint64_t track_coroutine_task(ResourceCleanupFn cleanup, const std::string &description = "") const
        {
            return track_resource(PluginResourceType::coroutine_task, std::move(cleanup), description);
        }

        uint64_t track_async_task(ResourceCleanupFn cleanup, const std::string &description = "") const
        {
            return track_resource(PluginResourceType::async_task, std::move(cleanup), description);
        }

        bool untrack_resource(uint64_t resource_id) const
        {
            if (ctx_.resource_guard) {
                return ctx_.resource_guard->untrack(resource_id);
            }
            return false;
        }

        HostEventSubscription subscribe_event(const std::string &event_name, HostEventHandler handler) const
        {
            if (!ctx_.has_capability(PluginPermission::use_event_bus, ctx_.event_bus) || !handler) {
                return 0;
            }
            auto token = ctx_.event_bus->subscribe(event_name, std::move(handler));
            if (token != 0 && ctx_.resource_guard) {
                auto *bus = ctx_.event_bus;
                track_resource(
                    PluginResourceType::event_subscription,
                    [bus, token]() {
                    if (bus) {
                        bus->unsubscribe(token);
                    }
                    },
                    "event:" + event_name);
            }
            return token;
        }

        HostSchedulerTaskId schedule_task(std::chrono::milliseconds delay,
                                          HostSchedulerCallback callback,
                                          const std::string &name = "") const
        {
            if (!ctx_.has_capability(PluginPermission::use_scheduler, ctx_.scheduler) || !callback) {
                return 0;
            }
            auto id = ctx_.scheduler->schedule_after(delay, std::move(callback), name);
            if (id != 0 && ctx_.resource_guard) {
                auto *sched = ctx_.scheduler;
                track_resource(
                    PluginResourceType::scheduler_task,
                    [sched, id]() {
                    if (sched) {
                        sched->cancel(id);
                    }
                    },
                    "task:" + name);
            }
            return id;
        }

        HostSchedulerTaskId schedule_interval_task(std::chrono::milliseconds interval,
                                                   HostSchedulerCallback callback,
                                                   const std::string &name = "") const
        {
            if (!ctx_.has_capability(PluginPermission::use_scheduler, ctx_.scheduler) || !callback) {
                return 0;
            }
            auto id = ctx_.scheduler->schedule_interval(interval, std::move(callback), name);
            if (id != 0 && ctx_.resource_guard) {
                auto *sched = ctx_.scheduler;
                track_resource(
                    PluginResourceType::scheduler_task,
                    [sched, id]() {
                    if (sched) {
                        sched->cancel(id);
                    }
                    },
                    "interval:" + name);
            }
            return id;
        }

        template <typename T>
        bool register_managed_service(const std::string &name,
                                      std::shared_ptr<T> service,
                                      const std::string &contract_id = {},
                                      int contract_version = 1) const
        {
            if (!ctx_.has_capability(PluginPermission::use_service_registry, ctx_.service_registry) || !service) {
                return false;
            }
            return ctx_.service_registry->register_managed_service(
                ctx_.plugin_name,
                name,
                std::move(service),
                contract_id,
                contract_version);
        }

        template <typename T>
        std::shared_ptr<T> find_plugin_service(const std::string &name) const
        {
            if (!ctx_.has_capability(PluginPermission::use_service_registry, ctx_.service_registry)) {
                return {};
            }
            return ctx_.service_registry->find_service_as<T>(name);
        }

        template <typename T>
        std::shared_ptr<T> find_host_service(const std::string &name) const
        {
            if (!ctx_.has_capability(PluginPermission::use_service_catalog, ctx_.service_catalog)) {
                return {};
            }
            return ctx_.service_catalog->get_service_as<T>(name);
        }

        HostTimerId schedule_network_timer(uint32_t delay_ms, HostTimerCallback callback) const
        {
            if (!ctx_.has_capability(PluginPermission::use_network_runtime, ctx_.network_runtime) || !callback) {
                return 0;
            }
            auto id = ctx_.network_runtime->schedule_timer(delay_ms, std::move(callback));
            if (id != 0 && ctx_.resource_guard) {
                auto *nr = ctx_.network_runtime;
                track_resource(
                    PluginResourceType::scheduler_task,
                    [nr, id]() {
                    if (nr) {
                        nr->cancel_timer(id);
                    }
                    },
                    "network_timer:" + std::to_string(id));
            }
            return id;
        }

        HostTimerId schedule_network_periodic_timer(uint32_t delay_ms, uint32_t interval_ms,
                                                    HostTimerCallback callback) const
        {
            if (!ctx_.has_capability(PluginPermission::use_network_runtime, ctx_.network_runtime) || !callback) {
                return 0;
            }
            auto id = ctx_.network_runtime->schedule_periodic_timer(delay_ms, interval_ms, std::move(callback));
            if (id != 0 && ctx_.resource_guard) {
                auto *nr = ctx_.network_runtime;
                track_resource(
                    PluginResourceType::scheduler_task,
                    [nr, id]() {
                    if (nr) {
                        nr->cancel_timer(id);
                    }
                    },
                    "network_periodic:" + std::to_string(id));
            }
            return id;
        }

        bool dispatch_on_network(std::function<void()> callback) const
        {
            if (!ctx_.has_capability(PluginPermission::use_network_runtime, ctx_.network_runtime) || !callback) {
                return false;
            }
            ctx_.network_runtime->dispatch(std::move(callback));
            return true;
        }

        const ExtensionPointEntry *resolve_extension_point(const std::string &name) const
        {
            if (!ctx_.extension_point_registry) {
                return nullptr;
            }
            return ctx_.extension_point_registry->find_one(name);
        }

        const ExtensionPointEntry *resolve_extension_contract(const std::string &contract_id) const
        {
            if (!ctx_.extension_point_registry) {
                return nullptr;
            }
            return ctx_.extension_point_registry->find_best_contract(contract_id);
        }

        std::vector<const ExtensionPointEntry *> resolve_all_extensions(const std::string &name) const
        {
            if (!ctx_.extension_point_registry) {
                return {};
            }
            return ctx_.extension_point_registry->find_by_name(name);
        }

        std::vector<const ExtensionPointEntry *> resolve_all_by_contract(const std::string &contract_id,
                                                                         int min_version = 1) const
        {
            if (!ctx_.extension_point_registry) {
                return {};
            }
            return ctx_.extension_point_registry->find_by_contract(contract_id, min_version);
        }

    private:
        const PluginContext &ctx_;
    };

} // namespace yuan::plugin

#endif
