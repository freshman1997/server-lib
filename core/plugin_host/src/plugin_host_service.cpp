#include "plugin_host_service.h"

#include "eventbus/event_bus.h"
#include "logger.h"
#include "plugin_event_bus_adapter.h"
#include "plugin_host_logger.h"
#include "plugin_host_scheduler.h"
#include "plugin_http_interceptor.h"
#include "plugin_permission_guard.h"
#include "plugin_redis_storage.h"
#include "plugin_resource_guard.h"
#include "plugin_service_catalog.h"
#include "plugin_service_registry_adapter.h"
#include "registry.h"
#include "plugin/plugin_events.h"
#include "plugin/plugin_manager.h"

#include <algorithm>
#include <utility>
#include <iostream>

namespace yuan::app
{

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
    event.worker_index = context.worker_index;
    event.is_worker_process = context.is_worker_process;
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

plugin::PluginContext PluginHostService::make_base_plugin_context(plugin::HostStorage *storage) const
{
    return plugin::PluginContext{
        runtime_context_.app_name,
        {},                                     // plugin_name (per-plugin)
        plugin_path_,                           // plugin_root_path
        {},                                     // plugin_config_path
        {},                                     // config
        to_plugin_run_mode(runtime_context_.run_mode),
        runtime_context_.worker_threads,
        runtime_context_.worker_index,
        runtime_context_.is_worker_process,
        event_bus_.get(),                       // event_bus
        logger_.get(),                          // logger
        service_catalog_.get(),                 // service_catalog
        scheduler_.get(),                       // scheduler
        service_registry_.get(),                // service_registry
        permission_guard_.get(),                // permission_guard
        resource_guard_.get(),                  // resource_guard
        http_interceptor_.get(),                // http_interceptor
        storage,                                // storage (per-plugin)
        plugin::PluginPermission::none          // granted_permissions
    };
}

plugin::HostStorage *PluginHostService::prepare_plugin_storage(const std::string &plugin_name)
{
    auto it = plugin_storages_.find(plugin_name);
    if (it != plugin_storages_.end()) {
        return it->second.get();
    }

    auto storage = std::make_unique<PluginRedisStorage>(plugin_name);
    auto *raw = storage.get();
    if (!storage->init()) {
        LOG_WARN("plugin storage for '{}' is present but backend is unavailable", plugin_name);
    }

    plugin_storages_.emplace(plugin_name, std::move(storage));
    return raw;
}

PluginHostService::PluginHostService(std::string plugin_path)
    : plugin_path_(std::move(plugin_path))
{
}

PluginHostService::PluginHostService(std::string plugin_path, std::vector<std::string> plugins)
    : plugin_path_(std::move(plugin_path))
    , plugin_names_(std::move(plugins))
{
}

void PluginHostService::set_plugin_path(std::string plugin_path)
{
    plugin_path_ = std::move(plugin_path);
}

const std::string& PluginHostService::plugin_path() const
{
    return plugin_path_;
}

bool PluginHostService::add_plugin(std::string plugin_name)
{
    if (plugin_name.empty()) {
        return false;
    }

    const auto it = std::find(plugin_names_.begin(), plugin_names_.end(), plugin_name);
    if (it != plugin_names_.end()) {
        return false;
    }

    plugin_names_.push_back(std::move(plugin_name));
    return true;
}

const std::vector<std::string>& PluginHostService::plugins() const
{
    return plugin_names_;
}

bool PluginHostService::load_plugin(const std::string &plugin_name)
{
    auto pluginManager = yuan::plugin::PluginManager::get_instance();

    auto *plugin_storage = prepare_plugin_storage(plugin_name);
    pluginManager->set_context(make_base_plugin_context(plugin_storage));

    if (!pluginManager->load(plugin_name)) {
        LOG_ERROR("runtime load plugin '{}' failed", plugin_name);
        if (runtime_context_.event_bus) {
            runtime_context_.event_bus->publish(
                plugin::events::plugin_load_failed,
                make_plugin_load_failed_event(runtime_context_, plugin_name, "runtime load failed"));
        }
        return false;
    }

    if (service_registry_) {
        auto *registry = static_cast<PluginServiceRegistryAdapter *>(service_registry_.get());
        if (!registry->init_plugin_services(plugin_name, pluginManager->plugin_context(plugin_name))) {
            registry->stop_plugin_services(plugin_name);
            if (http_interceptor_) {
                http_interceptor_->remove_by_plugin(plugin_name);
            }
            if (resource_guard_) {
                resource_guard_->cleanup_plugin(plugin_name);
            }
            registry->unregister_plugin_services(plugin_name);
            if (permission_guard_) {
                permission_guard_->revoke(plugin_name, plugin::PluginPermission::all);
            }
            pluginManager->release_plugin(plugin_name);
            plugin_storages_.erase(plugin_name);
            return false;
        }
        if (started_) {
            registry->start_plugin_services(plugin_name);
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

bool PluginHostService::unload_plugin(const std::string &plugin_name)
{
    auto it = std::find(loaded_plugins_.begin(), loaded_plugins_.end(), plugin_name);
    if (it == loaded_plugins_.end()) {
        return false;
    }

    auto pluginManager = yuan::plugin::PluginManager::get_instance();

    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(
            plugin::events::plugin_unloading,
            make_plugin_event(plugin_name));
    }

    if (service_registry_) {
        static_cast<PluginServiceRegistryAdapter *>(service_registry_.get())->stop_plugin_services(plugin_name);
    }

    if (http_interceptor_) {
        http_interceptor_->remove_by_plugin(plugin_name);
    }

    // 1. 通过资源守卫清理该插件注册的所有宿主资源 (定时任务、事件订阅等)
    //    注意: 这会在 release_plugin 之前执行, 确保插件 on_release 时
    //    部分资源已被宿主侧自动清理
    if (resource_guard_) {
        resource_guard_->cleanup_plugin(plugin_name);
    }

    // 2. 清理 HTTP 拦截器注册的中间件/路由回调
    if (http_interceptor_) {
        http_interceptor_->remove_by_plugin(plugin_name);
    }

    // 3. 注销该插件注册的所有服务
    if (service_registry_) {
        service_registry_->unregister_plugin_services(plugin_name);
    }

    // 4. 撤销该插件的所有权限
    if (permission_guard_) {
        permission_guard_->revoke(plugin_name, plugin::PluginPermission::all);
    }

    pluginManager->release_plugin(plugin_name);
    plugin_storages_.erase(plugin_name);
    loaded_plugins_.erase(it);

    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(
            plugin::events::plugin_unloaded,
            make_plugin_event(plugin_name));
    }
    return true;
}

std::vector<std::pair<std::string, bool>> PluginHostService::health_check_all()
{
    std::vector<std::pair<std::string, bool>> results;
    auto pluginManager = yuan::plugin::PluginManager::get_instance();

    for (const auto &name : loaded_plugins_) {
        auto *plugin = pluginManager->get_plugin(name);
        bool healthy = plugin && plugin->on_health_check();
        results.emplace_back(name, healthy);

        if (runtime_context_.event_bus) {
            plugin::PluginHealthCheckEvent event;
            static_cast<plugin::PluginEvent &>(event) = make_plugin_event(name);
            event.healthy = healthy;
            runtime_context_.event_bus->publish(plugin::events::plugin_health_checked, event);
        }
    }
    return results;
}

bool PluginHostService::health_check(const std::string &plugin_name)
{
    auto pluginManager = yuan::plugin::PluginManager::get_instance();
    auto *plugin = pluginManager->get_plugin(plugin_name);
    if (!plugin) {
        return false;
    }

    bool healthy = plugin->on_health_check();
    if (runtime_context_.event_bus) {
        plugin::PluginHealthCheckEvent event;
        static_cast<plugin::PluginEvent &>(event) = make_plugin_event(plugin_name);
        event.healthy = healthy;
        runtime_context_.event_bus->publish(plugin::events::plugin_health_checked, event);
    }
    return healthy;
}

bool PluginHostService::reload_config(const std::string &plugin_name)
{
    auto it = std::find(loaded_plugins_.begin(), loaded_plugins_.end(), plugin_name);
    if (it == loaded_plugins_.end()) {
        return false;
    }

    auto pluginManager = yuan::plugin::PluginManager::get_instance();
    auto *plugin = pluginManager->get_plugin(plugin_name);
    if (!plugin) {
        return false;
    }

    auto new_config = pluginManager->reload_plugin_config(plugin_name);
    if (!new_config.loaded()) {
        LOG_WARN("reload config for plugin '{}' failed: config not loaded", plugin_name);
        return false;
    }

    plugin->on_config_changed(new_config);

    if (runtime_context_.event_bus) {
        plugin::PluginConfigChangedEvent event;
        static_cast<plugin::PluginEvent &>(event) = make_plugin_event(plugin_name);
        event.config_path = plugin_path_ + plugin_name + ".json";
        runtime_context_.event_bus->publish(plugin::events::plugin_config_changed, event);
    }
    return true;
}

void PluginHostService::set_default_permissions(plugin::PluginPermission perm)
{
    if (permission_guard_) {
        static_cast<PluginPermissionGuard *>(permission_guard_.get())->set_default_permissions(perm);
    }
}

void PluginHostService::set_plugin_permissions(const std::string &plugin_name, plugin::PluginPermission perm)
{
    if (permission_guard_) {
        // 先撤销旧权限, 再授予新权限
        permission_guard_->revoke(plugin_name, plugin::PluginPermission::all);
        permission_guard_->grant(plugin_name, perm);
    }
}

void PluginHostService::set_http_server_accessor(std::function<void *()> accessor)
{
    pending_http_server_accessor_ = std::move(accessor);
    if (http_interceptor_) {
        static_cast<PluginHttpInterceptor *>(http_interceptor_.get())->set_server_accessor(
            [acc = pending_http_server_accessor_]() -> void * {
                return acc ? acc() : nullptr;
            }
        );
    }
}

void PluginHostService::set_http_installers(
    std::function<bool(std::shared_ptr<plugin::HttpMiddlewareCallback>, std::string)> middleware_installer,
    std::function<bool(std::shared_ptr<plugin::HttpRouteCallback>, std::string, std::string, std::string)> route_installer)
{
    pending_http_middleware_installer_ = std::move(middleware_installer);
    pending_http_route_installer_ = std::move(route_installer);
    if (http_interceptor_) {
        static_cast<PluginHttpInterceptor *>(http_interceptor_.get())->set_installers(
            pending_http_middleware_installer_,
            pending_http_route_installer_);
    }
}

void PluginHostService::set_runtime_context(const RuntimeContext &context)
{
    runtime_context_ = context;
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

plugin::PluginEvent PluginHostService::make_plugin_event(const std::string &plugin_name) const
{
    return make_plugin_event_impl(runtime_context_, plugin_name);
}

bool PluginHostService::init()
{
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

    // HTTP 拦截器: 设置 server accessor 为空 (实际桥接由 server 层完成)
    // 宿主可在 init 之后调用 set_http_server_accessor() 来注入 HttpServer 访问能力
    static_cast<PluginHttpInterceptor *>(http_interceptor_.get())->set_resource_guard(
        resource_guard_.get());
    if (pending_http_server_accessor_) {
        static_cast<PluginHttpInterceptor *>(http_interceptor_.get())->set_server_accessor(
            [acc = pending_http_server_accessor_]() -> void * {
                return acc ? acc() : nullptr;
            });
    }
    if (pending_http_middleware_installer_ || pending_http_route_installer_) {
        static_cast<PluginHttpInterceptor *>(http_interceptor_.get())->set_installers(
            pending_http_middleware_installer_,
            pending_http_route_installer_);
    }

    auto pluginManager = yuan::plugin::PluginManager::get_instance();
    pluginManager->set_plugin_path(plugin_path_);
    pluginManager->set_context(make_base_plugin_context(nullptr));

    loaded_plugins_.clear();
    for (const auto &pluginName : plugin_names_) {
        auto *plugin_storage = prepare_plugin_storage(pluginName);
        pluginManager->set_context(make_base_plugin_context(plugin_storage));

        if (!pluginManager->load(pluginName)) {
            LOG_ERROR("plugin host failed to load plugin '{}'", pluginName);
            if (runtime_context_.event_bus) {
                runtime_context_.event_bus->publish(
                    plugin::events::plugin_load_failed,
                    make_plugin_load_failed_event(runtime_context_, pluginName, "plugin manager load returned false"));
            }
            for (auto it = loaded_plugins_.rbegin(); it != loaded_plugins_.rend(); ++it) {
                if (service_registry_) {
                    static_cast<PluginServiceRegistryAdapter *>(service_registry_.get())->stop_plugin_services(*it);
                }
                if (http_interceptor_) {
                    http_interceptor_->remove_by_plugin(*it);
                }
                if (resource_guard_) {
                    resource_guard_->cleanup_plugin(*it);
                }
                if (service_registry_) {
                    service_registry_->unregister_plugin_services(*it);
                }
                if (permission_guard_) {
                    permission_guard_->revoke(*it, plugin::PluginPermission::all);
                }
                pluginManager->release_plugin(*it);
                plugin_storages_.erase(*it);
            }
            loaded_plugins_.clear();
            return false;
        }

        if (service_registry_) {
            auto *registry = static_cast<PluginServiceRegistryAdapter *>(service_registry_.get());
            if (!registry->init_plugin_services(pluginName, pluginManager->plugin_context(pluginName))) {
                LOG_ERROR("plugin host failed to init services for plugin '{}'", pluginName);
                registry->stop_plugin_services(pluginName);
                if (http_interceptor_) {
                    http_interceptor_->remove_by_plugin(pluginName);
                }
                if (resource_guard_) {
                    resource_guard_->cleanup_plugin(pluginName);
                }
                registry->unregister_plugin_services(pluginName);
                if (permission_guard_) {
                    permission_guard_->revoke(pluginName, plugin::PluginPermission::all);
                }
                pluginManager->release_plugin(pluginName);
                plugin_storages_.erase(pluginName);
                for (auto it = loaded_plugins_.rbegin(); it != loaded_plugins_.rend(); ++it) {
                    registry->stop_plugin_services(*it);
                    if (http_interceptor_) {
                        http_interceptor_->remove_by_plugin(*it);
                    }
                    if (resource_guard_) {
                        resource_guard_->cleanup_plugin(*it);
                    }
                    registry->unregister_plugin_services(*it);
                    if (permission_guard_) {
                        permission_guard_->revoke(*it, plugin::PluginPermission::all);
                    }
                    pluginManager->release_plugin(*it);
                    plugin_storages_.erase(*it);
                }
                loaded_plugins_.clear();
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

    if (service_registry_) {
        auto *registry = static_cast<PluginServiceRegistryAdapter *>(service_registry_.get());
        for (const auto &plugin_name : loaded_plugins_) {
            registry->start_plugin_services(plugin_name);
        }
    }

    started_ = true;
}

void PluginHostService::stop()
{
    if (service_registry_) {
        auto *registry = static_cast<PluginServiceRegistryAdapter *>(service_registry_.get());
        for (auto it = loaded_plugins_.rbegin(); it != loaded_plugins_.rend(); ++it) {
            registry->stop_plugin_services(*it);
        }
    }
    started_ = false;

    // 1. 先停调度器 — 防止回调在插件释放后仍触发
    //    shutdown 会等待当前正在执行的回调完成
    if (scheduler_) {
        static_cast<PluginHostScheduler *>(scheduler_.get())->shutdown();
    }

    // 2. 逆序释放插件 (此时调度器已停, 插件 on_release 中 cancel 调用安全返回)
    auto pluginManager = yuan::plugin::PluginManager::get_instance();

    for (auto it = loaded_plugins_.rbegin(); it != loaded_plugins_.rend(); ++it) {
        if (runtime_context_.event_bus) {
            runtime_context_.event_bus->publish(
                plugin::events::plugin_unloading,
                make_plugin_event(*it));
        }

        // 通过资源守卫清理所有宿主侧追踪的资源
        if (resource_guard_) {
            resource_guard_->cleanup_plugin(*it);
        }

        // 清理 HTTP 拦截器
        if (http_interceptor_) {
            http_interceptor_->remove_by_plugin(*it);
        }

        // 注销该插件注册的所有服务
        if (service_registry_) {
            service_registry_->unregister_plugin_services(*it);
        }

        // 撤销该插件的所有权限
        if (permission_guard_) {
            permission_guard_->revoke(*it, plugin::PluginPermission::all);
        }

        pluginManager->release_plugin(*it);
        plugin_storages_.erase(*it);
        if (runtime_context_.event_bus) {
            runtime_context_.event_bus->publish(
                plugin::events::plugin_unloaded,
                make_plugin_event(*it));
        }
    }
    loaded_plugins_.clear();

    // 3. 释放宿主接口
    resource_guard_.reset();
    scheduler_.reset();
    http_interceptor_.reset();
    plugin_storages_.clear();
    service_registry_.reset();
    permission_guard_.reset();
    logger_.reset();
    event_bus_.reset();
    service_catalog_.reset();
}

} // namespace yuan::app
