#include "plugin/plugin_lifecycle_manager.h"
#include "plugin/plugin.h"
#include "plugin/plugin_context.h"
#include "plugin/host_resource_guard.h"
#include "plugin/host_service_registry.h"
#include "plugin/host_http_interceptor.h"
#include "plugin/host_permission_guard.h"
#include "plugin/host_scheduler.h"
#include "plugin/host_event_bus.h"
#include "plugin/plugin_symbol_solver.h"

#include "logger.h"

namespace yuan::plugin
{

    PluginLifecycleManager::~PluginLifecycleManager()
    {
        unload_all();
    }

    void PluginLifecycleManager::set_resource_guard(HostResourceGuard * guard)
    {
        resource_guard_ = guard;
    }

    void PluginLifecycleManager::set_service_registry(HostServiceRegistry * registry)
    {
        service_registry_ = registry;
    }

    void PluginLifecycleManager::set_http_interceptor(HostHttpInterceptor * interceptor)
    {
        http_interceptor_ = interceptor;
    }

    void PluginLifecycleManager::set_permission_guard(HostPermissionGuard * guard)
    {
        permission_guard_ = guard;
    }

    void PluginLifecycleManager::set_scheduler(HostScheduler * scheduler)
    {
        scheduler_ = scheduler;
    }

    void PluginLifecycleManager::set_event_bus(HostEventBus * bus)
    {
        event_bus_ = bus;
    }

    void PluginLifecycleManager::set_call_guard(std::unique_ptr<PluginCallGuard> guard)
    {
        call_guard_ = std::move(guard);
    }

    bool PluginLifecycleManager::register_instance(const std::string & name,
                                                   Plugin * plugin,
                                                   void * library_handle)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (instances_.count(name)) {
            return false;
        }

        PluginInstance instance;
        instance.name = name;
        instance.plugin = plugin;
        instance.library_handle = library_handle;
        instance.state = PluginState::loaded;
        instance.context = nullptr;

        instances_[name] = std::move(instance);
        load_order_.push_back(name);
        return true;
    }

    PluginInstance *PluginLifecycleManager::find_instance(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        return it != instances_.end() ? &it->second : nullptr;
    }

    const PluginInstance *PluginLifecycleManager::find_instance(const std::string & name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        return it != instances_.end() ? &it->second : nullptr;
    }

    PluginState PluginLifecycleManager::state(const std::string & name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        return it != instances_.end() ? it->second.state : PluginState::unloaded;
    }

    bool PluginLifecycleManager::transition(const std::string & name, PluginState new_state)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return do_transition(name, new_state);
    }

    bool PluginLifecycleManager::do_transition(const std::string & name, PluginState new_state)
    {
        auto it = instances_.find(name);
        if (it == instances_.end()) {
            return false;
        }

        auto old_state = it->second.state;
        if (old_state == new_state) {
            return true;
        }

        if (!can_transition(old_state, new_state)) {
            LOG_WARN("plugin '{}' invalid state transition: {} -> {}",
                     name, to_string(old_state), to_string(new_state));
            return false;
        }

        it->second.state = new_state;
        LOG_INFO("plugin '{}' state: {} -> {}", name, to_string(old_state), to_string(new_state));

        notify_state_change(name, old_state, new_state);

        if (new_state == PluginState::faulted || new_state == PluginState::quarantined) {
            if (scheduler_) {
                scheduler_->cancel_by_prefix(name);
            }
        }

        if (new_state == PluginState::stopped) {
            do_cleanup_plugin(name);
        }

        return true;
    }

    bool PluginLifecycleManager::activate(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end()) {
            return false;
        }

        if (it->second.state == PluginState::initialized) {
            return do_transition(name, PluginState::active);
        }
        if (it->second.state == PluginState::degraded) {
            call_guard_->reset_faults(name);
            return do_transition(name, PluginState::active);
        }
        return false;
    }

    bool PluginLifecycleManager::fault(const std::string & name, const std::string & reason)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end()) {
            return false;
        }

        if (!is_operational(it->second.state)) {
            return false;
        }

        LOG_ERROR("plugin '{}' faulted: {}", name, reason);

        call_guard_->report_fault(name, "lifecycle::fault", reason);

        auto suggested = call_guard_->suggested_state(name);
        if (suggested == PluginState::quarantined) {
            return do_transition(name, PluginState::quarantined);
        }
        return do_transition(name, PluginState::faulted);
    }

    bool PluginLifecycleManager::quarantine(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return do_transition(name, PluginState::quarantined);
    }

    bool PluginLifecycleManager::degrade(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return do_transition(name, PluginState::degraded);
    }

    bool PluginLifecycleManager::recover(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end()) {
            return false;
        }

        if (it->second.state == PluginState::faulted) {
            call_guard_->reset_faults(name);
            return do_transition(name, PluginState::degraded);
        }
        if (it->second.state == PluginState::degraded) {
            call_guard_->reset_faults(name);
            return do_transition(name, PluginState::active);
        }
        return false;
    }

    bool PluginLifecycleManager::stop(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end()) {
            return false;
        }

        if (it->second.state == PluginState::stopped || it->second.state == PluginState::unloaded) {
            return true;
        }

        if (!is_operational(it->second.state) &&
            it->second.state != PluginState::faulted &&
            it->second.state != PluginState::quarantined &&
            it->second.state != PluginState::initialized) {
            return false;
        }

        do_transition(name, PluginState::stopping);

        auto *plugin = it->second.plugin;
        if (plugin) {
            try
            {
                plugin->on_release();
            }
            catch (const std::exception &ex)
            {
                LOG_ERROR("plugin '{}' on_release() threw: {}", name, ex.what());
                call_guard_->report_fault(name, "on_release", ex.what());
            }
            catch (...)
            {
                LOG_ERROR("plugin '{}' on_release() threw unknown exception", name);
                call_guard_->report_fault(name, "on_release", "unknown exception");
            }
        }

        return do_transition(name, PluginState::stopped);
    }

    bool PluginLifecycleManager::unload(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end()) {
            return false;
        }

        if (it->second.state != PluginState::stopped && it->second.state != PluginState::discovered) {
            LOG_WARN("plugin '{}' cannot unload from state {}", name, to_string(it->second.state));
            return false;
        }

        auto *plugin = it->second.plugin;
        auto *handle = it->second.library_handle;

        instances_.erase(it);

        load_order_.erase(
            std::remove(load_order_.begin(), load_order_.end(), name),
            load_order_.end());

        delete plugin;
        if (handle) {
            PluginSymbolSolver::release_native_lib(handle);
        }

        LOG_INFO("plugin '{}' unloaded", name);
        return true;
    }

    void PluginLifecycleManager::stop_all()
    {
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            names = load_order_;
        }

        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            stop(*it);
        }
    }

    void PluginLifecycleManager::unload_all()
    {
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            names = load_order_;
        }

        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            stop(*it);
            unload(*it);
        }
    }

    std::vector<std::string> PluginLifecycleManager::active_plugins() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        for (const auto &name : load_order_) {
            auto it = instances_.find(name);
            if (it != instances_.end() && is_operational(it->second.state)) {
                result.push_back(name);
            }
        }
        return result;
    }

    std::vector<std::string> PluginLifecycleManager::all_plugins() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return load_order_;
    }

    PluginCallGuard &PluginLifecycleManager::call_guard()
    {
        return *call_guard_;
    }

    const PluginCallGuard &PluginLifecycleManager::call_guard() const
    {
        return *call_guard_;
    }

    void PluginLifecycleManager::set_state_change_callback(StateChangeCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_change_callback_ = std::move(callback);
    }

    void PluginLifecycleManager::set_context(const std::string & name, PluginContext * context)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it != instances_.end()) {
            it->second.context = context;
        }
    }

    bool PluginLifecycleManager::accepts_callbacks(const std::string & name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end()) {
            return false;
        }
        return plugin::accepts_callbacks(it->second.state);
    }

    void PluginLifecycleManager::do_cleanup_plugin(const std::string & name)
    {
        if (http_interceptor_) {
            http_interceptor_->remove_by_plugin(name);
        }

        if (resource_guard_) {
            resource_guard_->cleanup_plugin(name);
        }

        if (service_registry_) {
            service_registry_->unregister_plugin_services(name);
        }

        if (permission_guard_) {
            permission_guard_->revoke(name, PluginPermission::all);
        }
    }

    void PluginLifecycleManager::notify_state_change(const std::string & name,
                                                     PluginState old_state,
                                                     PluginState new_state)
    {
        if (state_change_callback_) {
            state_change_callback_(name, old_state, new_state);
        }
    }

} // namespace yuan::plugin
