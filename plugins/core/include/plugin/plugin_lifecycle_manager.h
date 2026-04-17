#ifndef __YUAN_PLUGIN_PLUGIN_LIFECYCLE_MANAGER_H__
#define __YUAN_PLUGIN_PLUGIN_LIFECYCLE_MANAGER_H__

#include "plugin/plugin_call_guard.h"
#include "plugin/plugin_state.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::plugin
{

    class Plugin;
    class PluginContext;
    class HostResourceGuard;
    class HostServiceRegistry;
    class HostHttpInterceptor;
    class HostPermissionGuard;
    class HostScheduler;
    class HostEventBus;

    struct PluginInstance
    {
        std::string name;
        Plugin *plugin = nullptr;
        void *library_handle = nullptr;
        PluginState state = PluginState::discovered;
        PluginContext *context = nullptr;
    };

    class PluginLifecycleManager
    {
    public:
        using StateChangeCallback = std::function<void(const std::string &plugin_name,
                                                       PluginState old_state,
                                                       PluginState new_state)>;

        struct Config
        {
            PluginCallGuard::Config call_guard_config;
        };

        PluginLifecycleManager()
            : config_(Config{}), call_guard_(std::make_unique<PluginCallGuard>())
        {
        }

        explicit PluginLifecycleManager(Config config)
            : config_(std::move(config)), call_guard_(std::make_unique<PluginCallGuard>(config_.call_guard_config))
        {
        }

        ~PluginLifecycleManager();

        void set_resource_guard(HostResourceGuard *guard);
        void set_service_registry(HostServiceRegistry *registry);
        void set_http_interceptor(HostHttpInterceptor *interceptor);
        void set_permission_guard(HostPermissionGuard *guard);
        void set_scheduler(HostScheduler *scheduler);
        void set_event_bus(HostEventBus *bus);
        void set_call_guard(std::unique_ptr<PluginCallGuard> guard);

        bool register_instance(const std::string &name, Plugin *plugin, void *library_handle);

        PluginInstance *find_instance(const std::string &name);
        const PluginInstance *find_instance(const std::string &name) const;

        PluginState state(const std::string &name) const;
        bool transition(const std::string &name, PluginState new_state);

        bool activate(const std::string &name);
        bool fault(const std::string &name, const std::string &reason);
        bool quarantine(const std::string &name);
        bool degrade(const std::string &name);
        bool recover(const std::string &name);
        bool stop(const std::string &name);
        bool unload(const std::string &name);

        void stop_all();
        void unload_all();

        std::vector<std::string> active_plugins() const;
        std::vector<std::string> all_plugins() const;

        PluginCallGuard &call_guard();
        const PluginCallGuard &call_guard() const;

        void set_state_change_callback(StateChangeCallback callback);

        void set_context(const std::string &name, PluginContext *context);

        bool accepts_callbacks(const std::string &name) const;

    private:
        bool do_transition(const std::string &name, PluginState new_state);
        void do_cleanup_plugin(const std::string &name);
        void notify_state_change(const std::string &name, PluginState old_state, PluginState new_state);

        Config config_;
        std::unique_ptr<PluginCallGuard> call_guard_;
        HostResourceGuard *resource_guard_ = nullptr;
        HostServiceRegistry *service_registry_ = nullptr;
        HostHttpInterceptor *http_interceptor_ = nullptr;
        HostPermissionGuard *permission_guard_ = nullptr;
        HostScheduler *scheduler_ = nullptr;
        HostEventBus *event_bus_ = nullptr;

        mutable std::mutex mutex_;
        std::unordered_map<std::string, PluginInstance> instances_;
        std::vector<std::string> load_order_;

        StateChangeCallback state_change_callback_;
    };

} // namespace yuan::plugin

#endif
