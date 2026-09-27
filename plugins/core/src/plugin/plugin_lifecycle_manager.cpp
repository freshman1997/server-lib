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
    namespace
    {
        auto &current_thread_calls()
        {
            // Avoid cross-DLL thread-local destructor ordering during native plugin unload.
            thread_local auto *calls = new std::unordered_map<std::string, std::size_t>();
            return *calls;
        }
    }

    PluginLifecycleManager::CallLease::CallLease(PluginLifecycleManager *manager,
                                                 std::string plugin_name,
                                                 Plugin *plugin,
                                                 PluginState state)
        : manager_(manager), plugin_name_(std::move(plugin_name)), plugin_(plugin), state_(state)
    {
    }

    PluginLifecycleManager::CallLease::CallLease(CallLease &&other) noexcept
        : manager_(other.manager_), plugin_name_(std::move(other.plugin_name_)),
          plugin_(other.plugin_), state_(other.state_)
    {
        other.manager_ = nullptr;
        other.plugin_ = nullptr;
    }

    PluginLifecycleManager::CallLease &PluginLifecycleManager::CallLease::operator=(CallLease &&other) noexcept
    {
        if (this != &other) {
            reset();
            manager_ = other.manager_;
            plugin_name_ = std::move(other.plugin_name_);
            plugin_ = other.plugin_;
            state_ = other.state_;
            other.manager_ = nullptr;
            other.plugin_ = nullptr;
        }
        return *this;
    }

    PluginLifecycleManager::CallLease::~CallLease()
    {
        reset();
    }

    void PluginLifecycleManager::CallLease::reset()
    {
        if (manager_) {
            manager_->release_call(plugin_name_);
            manager_ = nullptr;
            plugin_ = nullptr;
        }
    }

    PluginLifecycleManager::~PluginLifecycleManager()
    {
        unload_all();
    }

    void PluginLifecycleManager::set_resource_guard(HostResourceGuard * guard)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resource_guard_ = guard;
    }

    void PluginLifecycleManager::set_service_registry(HostServiceRegistry * registry)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        service_registry_ = registry;
    }

    void PluginLifecycleManager::set_http_interceptor(HostHttpInterceptor * interceptor)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        http_interceptor_ = interceptor;
    }

    void PluginLifecycleManager::set_permission_guard(HostPermissionGuard * guard)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        permission_guard_ = guard;
    }

    void PluginLifecycleManager::set_scheduler(HostScheduler * scheduler)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_ = scheduler;
    }

    void PluginLifecycleManager::set_event_bus(HostEventBus * bus)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event_bus_ = bus;
    }

    void PluginLifecycleManager::set_call_guard(std::unique_ptr<PluginCallGuard> guard)
    {
        std::lock_guard<std::mutex> lock(mutex_);
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
        PluginState old_state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = instances_.find(name);
            if (it == instances_.end()) return false;
            old_state = it->second.state;
            if (!do_transition(name, new_state)) return false;
        }
        if (old_state != new_state) run_transition_effects(name, old_state, new_state);
        return true;
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
        return true;
    }

    bool PluginLifecycleManager::activate(const std::string & name)
    {
        auto current = state(name);
        if (current == PluginState::initialized) {
            return transition(name, PluginState::active);
        }
        if (current == PluginState::degraded) {
            call_guard_->reset_faults(name);
            return transition(name, PluginState::active);
        }
        return false;
    }

    bool PluginLifecycleManager::fault(const std::string & name, const std::string & reason)
    {
        auto current = state(name);
        if (!is_operational(current) && current != PluginState::faulted) {
            return false;
        }

        LOG_ERROR("plugin '{}' faulted: {}", name, reason);

        call_guard_->report_fault(name, "lifecycle::fault", reason);
        return apply_recorded_fault_policy(name);
    }

    bool PluginLifecycleManager::apply_recorded_fault_policy(const std::string &name)
    {
        auto suggested = call_guard_->suggested_state(name);
        if (suggested == PluginState::quarantined) {
            return transition(name, PluginState::quarantined);
        }
        if (suggested == PluginState::faulted) {
            return transition(name, PluginState::faulted);
        }
        if (suggested == PluginState::degraded) {
            return transition(name, PluginState::degraded);
        }
        return true;
    }

    bool PluginLifecycleManager::quarantine(const std::string & name)
    {
        return transition(name, PluginState::quarantined);
    }

    bool PluginLifecycleManager::degrade(const std::string & name)
    {
        return transition(name, PluginState::degraded);
    }

    bool PluginLifecycleManager::recover(const std::string & name)
    {
        auto current = state(name);
        if (current == PluginState::faulted) {
            return transition(name, PluginState::degraded);
        }
        if (current == PluginState::degraded) {
            call_guard_->reset_faults(name);
            return transition(name, PluginState::active);
        }
        return false;
    }

    bool PluginLifecycleManager::stop(const std::string & name)
    {
        if (current_thread_calls().count(name)) {
            LOG_WARN("plugin '{}' cannot stop itself from an active callback", name);
            return false;
        }
        Plugin *plugin = nullptr;
        PluginState old_state;
        bool release_plugin = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = instances_.find(name);
            if (it == instances_.end()) return false;
            old_state = it->second.state;
            if (old_state == PluginState::stopped || old_state == PluginState::unloaded) return true;
            if (old_state == PluginState::loaded || old_state == PluginState::discovered) {
                it->second.accepting_calls = false;
                do_transition(name, PluginState::stopped);
            } else {
                if (!is_operational(old_state) && old_state != PluginState::faulted &&
                    old_state != PluginState::quarantined && old_state != PluginState::initialized) return false;
                it->second.accepting_calls = false;
                do_transition(name, PluginState::stopping);
                plugin = it->second.plugin;
                release_plugin = true;
            }
        }
        if (!release_plugin) {
            run_transition_effects(name, old_state, PluginState::stopped);
            return true;
        }

        run_transition_effects(name, old_state, PluginState::stopping);
        // Remove callback producers before waiting for callbacks already in flight.
        do_cleanup_plugin(name);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            calls_drained_.wait(lock, [&]() {
                auto it = instances_.find(name);
                return it == instances_.end() || it->second.active_calls == 0;
            });
        }
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

        return transition(name, PluginState::stopped);
    }

    bool PluginLifecycleManager::unload(const std::string & name)
    {
        Plugin *plugin = nullptr;
        void *handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = instances_.find(name);
            if (it == instances_.end()) return false;
            if (it->second.state != PluginState::stopped && it->second.state != PluginState::discovered) {
                LOG_WARN("plugin '{}' cannot unload from state {}", name, to_string(it->second.state));
                return false;
            }
            if (it->second.active_calls != 0) return false;
            plugin = it->second.plugin;
            handle = it->second.library_handle;
            instances_.erase(it);
            load_order_.erase(std::remove(load_order_.begin(), load_order_.end(), name), load_order_.end());
        }

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
        return it->second.accepting_calls && plugin::accepts_callbacks(it->second.state);
    }

    PluginLifecycleManager::CallLease PluginLifecycleManager::acquire_call(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second.accepting_calls ||
            !plugin::accepts_callbacks(it->second.state) || !it->second.plugin) {
            return {};
        }
        ++it->second.active_calls;
        ++current_thread_calls()[name];
        return CallLease(this, name, it->second.plugin, it->second.state);
    }

    void PluginLifecycleManager::release_call(const std::string &name)
    {
        auto &thread_calls = current_thread_calls();
        auto thread_it = thread_calls.find(name);
        if (thread_it != thread_calls.end() && --thread_it->second == 0) {
            thread_calls.erase(thread_it);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it != instances_.end() && it->second.active_calls > 0 && --it->second.active_calls == 0) {
            calls_drained_.notify_all();
        }
    }

    void PluginLifecycleManager::do_cleanup_plugin(const std::string & name)
    {
        HostHttpInterceptor *http_interceptor;
        HostResourceGuard *resource_guard;
        HostServiceRegistry *service_registry;
        HostPermissionGuard *permission_guard;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            http_interceptor = http_interceptor_;
            resource_guard = resource_guard_;
            service_registry = service_registry_;
            permission_guard = permission_guard_;
        }
        if (http_interceptor) {
            http_interceptor->remove_by_plugin(name);
        }

        if (resource_guard) {
            resource_guard->cleanup_plugin(name);
        }

        if (service_registry) {
            service_registry->unregister_plugin_services(name);
        }

        if (permission_guard) {
            permission_guard->revoke(name, PluginPermission::all);
        }
    }

    void PluginLifecycleManager::notify_state_change(const std::string & name,
                                                     PluginState old_state,
                                                     PluginState new_state)
    {
        StateChangeCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = state_change_callback_;
        }
        if (callback) {
            callback(name, old_state, new_state);
        }
    }

    void PluginLifecycleManager::run_transition_effects(const std::string &name,
                                                        PluginState old_state,
                                                        PluginState new_state)
    {
        LOG_INFO("plugin '{}' state: {} -> {}", name, to_string(old_state), to_string(new_state));
        notify_state_change(name, old_state, new_state);
        if (new_state == PluginState::faulted || new_state == PluginState::quarantined) {
            HostScheduler *scheduler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                scheduler = scheduler_;
            }
            if (scheduler) scheduler->cancel_by_prefix(name);
        }
        if (new_state == PluginState::stopped) do_cleanup_plugin(name);
    }

} // namespace yuan::plugin
