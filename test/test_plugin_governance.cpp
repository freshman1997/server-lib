#include "plugin_resource_guard.h"
#include "plugin_service_registry_adapter.h"
#include "plugin/plugin_call_guard.h"
#include "plugin/plugin_context.h"
#include "plugin/plugin_events.h"
#include "plugin/plugin_lifecycle_manager.h"
#include "plugin/plugin_manifest.h"
#include "plugin/plugin_meta.h"
#include "plugin/plugin_state.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{

    void require(bool condition, const std::string &message)
    {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    class FakeEventBus : public yuan::plugin::HostEventBus
    {
    public:
        yuan::plugin::HostEventSubscription subscribe(const std::string &, yuan::plugin::HostEventHandler) override
        {
            return 1;
        }
        bool unsubscribe(yuan::plugin::HostEventSubscription) override
        {
            return true;
        }
        void publish(std::string, std::any) override
        {
        }
    };

    class FakeScheduler : public yuan::plugin::HostScheduler
    {
    public:
        yuan::plugin::HostSchedulerTaskId schedule_after(std::chrono::milliseconds,
                                                         yuan::plugin::HostSchedulerCallback,
                                                         const std::string &) override
        {
            return 0;
        }
        yuan::plugin::HostSchedulerTaskId schedule_interval(std::chrono::milliseconds,
                                                            yuan::plugin::HostSchedulerCallback,
                                                            const std::string &) override
        {
            return 0;
        }
        bool cancel(yuan::plugin::HostSchedulerTaskId) override
        {
            return true;
        }
        void cancel_by_prefix(const std::string &) override
        {
        }
        bool is_running() const override
        {
            return true;
        }
    };

    class StubPlugin : public yuan::plugin::Plugin
    {
    public:
        void on_loaded() override
        {
        }
        bool on_init(const yuan::plugin::PluginContext &) override
        {
            return true;
        }
        void on_release() override
        {
        }
    };

    void test_state_machine_transitions()
    {
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::discovered, yuan::plugin::PluginState::loaded),
                "discovered -> loaded should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::loaded, yuan::plugin::PluginState::initialized),
                "loaded -> initialized should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::initialized, yuan::plugin::PluginState::active),
                "initialized -> active should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::active, yuan::plugin::PluginState::degraded),
                "active -> degraded should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::active, yuan::plugin::PluginState::faulted),
                "active -> faulted should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::active, yuan::plugin::PluginState::stopping),
                "active -> stopping should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::faulted, yuan::plugin::PluginState::quarantined),
                "faulted -> quarantined should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::faulted, yuan::plugin::PluginState::degraded),
                "faulted -> degraded should be valid (recovery)");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::degraded, yuan::plugin::PluginState::active),
                "degraded -> active should be valid (recovery)");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::stopping, yuan::plugin::PluginState::stopped),
                "stopping -> stopped should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::stopped, yuan::plugin::PluginState::unloaded),
                "stopped -> unloaded should be valid");

        require(!yuan::plugin::can_transition(yuan::plugin::PluginState::unloaded, yuan::plugin::PluginState::active),
                "unloaded -> active should be invalid");
        require(!yuan::plugin::can_transition(yuan::plugin::PluginState::discovered, yuan::plugin::PluginState::active),
                "discovered -> active should be invalid");
        require(!yuan::plugin::can_transition(yuan::plugin::PluginState::active, yuan::plugin::PluginState::discovered),
                "active -> discovered should be invalid");
        require(!yuan::plugin::can_transition(yuan::plugin::PluginState::quarantined, yuan::plugin::PluginState::active),
                "quarantined -> active should be invalid");
    }

    void test_operational_and_callback_states()
    {
        require(yuan::plugin::is_operational(yuan::plugin::PluginState::active), "active should be operational");
        require(yuan::plugin::is_operational(yuan::plugin::PluginState::degraded), "degraded should be operational");
        require(!yuan::plugin::is_operational(yuan::plugin::PluginState::faulted), "faulted should not be operational");
        require(!yuan::plugin::is_operational(yuan::plugin::PluginState::quarantined), "quarantined should not be operational");
        require(!yuan::plugin::is_operational(yuan::plugin::PluginState::stopped), "stopped should not be operational");

        require(yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::active), "active should accept callbacks");
        require(yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::degraded), "degraded should accept callbacks");
        require(yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::initialized), "initialized should accept callbacks");
        require(!yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::faulted), "faulted should not accept callbacks");
        require(!yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::quarantined), "quarantined should not accept callbacks");
        require(!yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::stopped), "stopped should not accept callbacks");
    }

    void test_call_guard_fault_accumulation()
    {
        yuan::plugin::PluginCallGuard guard;

        require(guard.fault_count("test_plugin") == 0, "initial fault count should be 0");
        require(guard.suggested_state("test_plugin") == yuan::plugin::PluginState::active,
                "suggested state with 0 faults should be active");

        guard.guarded_call_void("test_plugin", yuan::plugin::PluginState::active, "test_call",
                                []() { throw std::runtime_error("boom"); });
        require(guard.fault_count("test_plugin") == 1, "fault count should be 1 after first exception");
        require(guard.suggested_state("test_plugin") == yuan::plugin::PluginState::degraded,
                "suggested state with 1 fault should be degraded");

        guard.guarded_call_void("test_plugin", yuan::plugin::PluginState::active, "test_call2",
                                []() { throw std::runtime_error("boom2"); });
        guard.guarded_call_void("test_plugin", yuan::plugin::PluginState::active, "test_call3",
                                []() { throw std::runtime_error("boom3"); });
        require(guard.fault_count("test_plugin") == 3, "fault count should be 3 after 3 exceptions");
        require(guard.suggested_state("test_plugin") == yuan::plugin::PluginState::faulted,
                "suggested state with 3 faults should be faulted");

        guard.guarded_call_void("test_plugin", yuan::plugin::PluginState::active, "test_call4",
                                []() { throw std::runtime_error("boom4"); });
        guard.guarded_call_void("test_plugin", yuan::plugin::PluginState::active, "test_call5",
                                []() { throw std::runtime_error("boom5"); });
        require(guard.fault_count("test_plugin") == 5, "fault count should be 5 after 5 exceptions");
        require(guard.suggested_state("test_plugin") == yuan::plugin::PluginState::quarantined,
                "suggested state with 5 faults should be quarantined");

        guard.reset_faults("test_plugin");
        require(guard.fault_count("test_plugin") == 0, "fault count should be 0 after reset");
    }

    void test_call_guard_blocks_faulted_plugin()
    {
        yuan::plugin::PluginCallGuard guard;

        bool called = false;
        bool result = guard.guarded_call_void("faulted_plugin", yuan::plugin::PluginState::faulted, "blocked_call",
                                              [&called]() { called = true; });
        require(!result, "guarded_call_void should return false for faulted plugin");
        require(!called, "callback should not execute for faulted plugin");

        called = false;
        result = guard.guarded_call_void("quarantined_plugin", yuan::plugin::PluginState::quarantined, "blocked_call",
                                         [&called]() { called = true; });
        require(!result, "guarded_call_void should return false for quarantined plugin");
        require(!called, "callback should not execute for quarantined plugin");

        called = false;
        result = guard.guarded_call_void("active_plugin", yuan::plugin::PluginState::active, "allowed_call",
                                         [&called]() { called = true; });
        require(result, "guarded_call_void should return true for active plugin");
        require(called, "callback should execute for active plugin");
    }

    void test_call_guard_fault_handler()
    {
        yuan::plugin::PluginCallGuard guard;

        yuan::plugin::FaultEvent last_event;
        int handler_calls = 0;

        guard.set_fault_handler([&](const yuan::plugin::FaultEvent &event) {
        last_event = event;
        ++handler_calls;
        });

        guard.guarded_call_void("handler_test", yuan::plugin::PluginState::active, "site1",
                                []() { throw std::runtime_error("err"); });

        require(handler_calls == 1, "fault handler should be called once");
        require(last_event.plugin_name == "handler_test", "fault event should contain plugin name");
        require(last_event.call_site == "site1", "fault event should contain call site");
        require(last_event.error_message == "err", "fault event should contain error message");
    }

    void test_call_guard_custom_thresholds()
    {
        yuan::plugin::PluginCallGuard::Config config;
        config.fault_threshold = 2;
        config.quarantine_threshold = 4;

        yuan::plugin::PluginCallGuard guard(config);

        guard.guarded_call_void("custom", yuan::plugin::PluginState::active, "call",
                                []() { throw std::runtime_error("e"); });
        require(guard.suggested_state("custom") == yuan::plugin::PluginState::degraded,
                "1 fault with threshold=2 should suggest degraded");

        guard.guarded_call_void("custom", yuan::plugin::PluginState::active, "call",
                                []() { throw std::runtime_error("e"); });
        require(guard.suggested_state("custom") == yuan::plugin::PluginState::faulted,
                "2 faults with threshold=2 should suggest faulted");

        guard.guarded_call_void("custom", yuan::plugin::PluginState::active, "call",
                                []() { throw std::runtime_error("e"); });
        guard.guarded_call_void("custom", yuan::plugin::PluginState::active, "call",
                                []() { throw std::runtime_error("e"); });
        require(guard.suggested_state("custom") == yuan::plugin::PluginState::quarantined,
                "4 faults with threshold=4 should suggest quarantined");
    }

    void test_lifecycle_manager_state_transitions()
    {
        yuan::plugin::PluginLifecycleManager mgr;

        auto plugin = new StubPlugin();
        require(mgr.register_instance("stub", plugin, nullptr),
                "register_instance should succeed for new plugin");
        require(mgr.state("stub") == yuan::plugin::PluginState::loaded,
                "newly registered plugin should be in loaded state");

        require(mgr.transition("stub", yuan::plugin::PluginState::initialized),
                "loaded -> initialized should succeed");
        require(mgr.state("stub") == yuan::plugin::PluginState::initialized,
                "plugin should be in initialized state");

        require(mgr.transition("stub", yuan::plugin::PluginState::active),
                "initialized -> active should succeed");
        require(mgr.state("stub") == yuan::plugin::PluginState::active,
                "plugin should be in active state");

        require(mgr.accepts_callbacks("stub"), "active plugin should accept callbacks");

        require(mgr.fault("stub", "test fault"), "fault should succeed on operational plugin");
        require(mgr.state("stub") == yuan::plugin::PluginState::degraded,
                "plugin with 1 fault should be degraded");
        require(mgr.accepts_callbacks("stub"), "degraded plugin should still accept callbacks");

        require(mgr.recover("stub"), "recovery should succeed from degraded");
        require(mgr.state("stub") == yuan::plugin::PluginState::active,
                "recovered plugin should be active");
        require(mgr.call_guard().fault_count("stub") == 0,
                "fault count should be reset after recovery");

        require(mgr.stop("stub"), "stop should succeed for active plugin");
        require(mgr.state("stub") == yuan::plugin::PluginState::stopped,
                "stopped plugin should be in stopped state");
        require(!mgr.accepts_callbacks("stub"), "stopped plugin should not accept callbacks");

        mgr.unload("stub");
    }

    void test_lifecycle_manager_fault_escalation()
    {
        yuan::plugin::PluginLifecycleManager::Config config;
        config.call_guard_config.fault_threshold = 2;
        config.call_guard_config.quarantine_threshold = 4;

        yuan::plugin::PluginLifecycleManager mgr(config);

        auto plugin = new StubPlugin();
        mgr.register_instance("escalation", plugin, nullptr);
        mgr.transition("escalation", yuan::plugin::PluginState::initialized);
        mgr.transition("escalation", yuan::plugin::PluginState::active);

        mgr.fault("escalation", "fault1");
        require(mgr.state("escalation") == yuan::plugin::PluginState::degraded,
                "1 fault should cause degraded");

        mgr.fault("escalation", "fault2");
        require(mgr.state("escalation") == yuan::plugin::PluginState::faulted,
                "2 faults should cause faulted");
        require(!mgr.accepts_callbacks("escalation"), "faulted plugin should not accept callbacks");

        mgr.recover("escalation");
        require(mgr.state("escalation") == yuan::plugin::PluginState::degraded,
                "recovery from faulted should go to degraded");

        mgr.fault("escalation", "fault3");
        mgr.fault("escalation", "fault4");
        require(mgr.state("escalation") == yuan::plugin::PluginState::quarantined,
                "4 faults should cause quarantined");
        require(!mgr.accepts_callbacks("escalation"), "quarantined plugin should not accept callbacks");

        require(!mgr.recover("escalation"), "recovery should fail for quarantined plugin");
        require(mgr.state("escalation") == yuan::plugin::PluginState::quarantined,
                "quarantined plugin should stay quarantined after failed recovery");

        mgr.stop("escalation");
        mgr.unload("escalation");
    }

    void test_lifecycle_manager_state_change_callback()
    {
        yuan::plugin::PluginLifecycleManager mgr;

        std::vector<std::pair<yuan::plugin::PluginState, yuan::plugin::PluginState> > transitions;

        mgr.set_state_change_callback([&](const std::string &name,
                                          yuan::plugin::PluginState old_state,
                                          yuan::plugin::PluginState new_state) {
        require(name == "cb_test", "callback should receive correct plugin name");
        transitions.push_back({old_state, new_state});
        });

        auto plugin = new StubPlugin();
        mgr.register_instance("cb_test", plugin, nullptr);
        mgr.transition("cb_test", yuan::plugin::PluginState::initialized);
        mgr.transition("cb_test", yuan::plugin::PluginState::active);

        require(transitions.size() == 2, "should have recorded 2 transitions");
        require(transitions[0].first == yuan::plugin::PluginState::loaded &&
                    transitions[0].second == yuan::plugin::PluginState::initialized,
                "first transition should be loaded -> initialized");
        require(transitions[1].first == yuan::plugin::PluginState::initialized &&
                    transitions[1].second == yuan::plugin::PluginState::active,
                "second transition should be initialized -> active");

        mgr.stop("cb_test");
        mgr.unload("cb_test");
    }

    void test_manifest_from_meta()
    {
        yuan::plugin::PluginMeta meta;
        meta.name = "test_plugin";
        meta.version = "2.0.0";
        meta.author = "test_author";
        meta.description = "test desc";
        meta.api_version = 3;
        meta.required_permissions = yuan::plugin::PluginPermission::use_event_bus | yuan::plugin::PluginPermission::use_logger;
        meta.depends_on = { "dep1", "dep2" };

        auto manifest = meta.to_manifest();

        require(manifest.plugin_id == "test_plugin", "manifest plugin_id should match meta name");
        require(manifest.name == "test_plugin", "manifest name should match meta name");
        require(manifest.version == "2.0.0", "manifest version should match meta version");
        require(manifest.author == "test_author", "manifest author should match meta author");
        require(manifest.description == "test desc", "manifest description should match meta description");
        require(manifest.api_version == 3, "manifest api_version should match meta api_version");
        require(yuan::plugin::has_permission(manifest.required_permissions, yuan::plugin::PluginPermission::use_event_bus),
                "manifest should preserve use_event_bus permission");
        require(yuan::plugin::has_permission(manifest.required_permissions, yuan::plugin::PluginPermission::use_logger),
                "manifest should preserve use_logger permission");
        require(manifest.depends_on.size() == 2, "manifest depends_on should be preserved");
        require(manifest.run_mode == yuan::plugin::PluginRunMode::unknown,
                "manifest run_mode should default to unknown");
        require(manifest.extension_points.empty(), "manifest extension_points should default to empty");
    }

    void test_event_descriptors()
    {
        require(std::string(yuan::plugin::event_descriptors::discovered.name) == "plugin.discovered",
                "discovered event name should match");
        require(std::string(yuan::plugin::event_descriptors::discovered.category) == "lifecycle",
                "discovered event category should be lifecycle");
        require(yuan::plugin::event_descriptors::discovered.scope == yuan::plugin::EventScope::host_internal,
                "discovered event scope should be host_internal");
        require(yuan::plugin::event_descriptors::discovered.delivery_semantics == yuan::plugin::EventDeliverySemantics::sync,
                "discovered event should be sync");

        require(std::string(yuan::plugin::event_descriptors::faulted.name) == "plugin.faulted",
                "faulted event name should match");
        require(yuan::plugin::event_descriptors::faulted.delivery_semantics == yuan::plugin::EventDeliverySemantics::sync,
                "faulted event should be sync delivery");

        require(yuan::plugin::event_descriptors::service_registered.required_permission == yuan::plugin::PluginPermission::use_service_registry,
                "service_registered event should require use_service_registry permission");

        require(yuan::plugin::event_descriptors::config_changed.scope == yuan::plugin::EventScope::plugin_local,
                "config_changed event should be plugin_local scope");
    }

    void test_capability_enforcement()
    {
        yuan::plugin::PluginContext ctx;
        ctx.plugin_name = "cap-test";
        ctx.granted_permissions = yuan::plugin::PluginPermission::none;

        require(!ctx.can_use(yuan::plugin::PluginPermission::use_event_bus),
                "plugin with none permission should not be able to use event_bus");

        ctx.granted_permissions = yuan::plugin::PluginPermission::use_event_bus;
        require(ctx.can_use(yuan::plugin::PluginPermission::use_event_bus),
                "plugin with use_event_bus should be able to use event_bus");
        require(!ctx.can_use(yuan::plugin::PluginPermission::use_scheduler),
                "plugin with only use_event_bus should not be able to use scheduler");

        FakeEventBus event_bus;
        ctx.event_bus = &event_bus;
        require(ctx.has_capability(yuan::plugin::PluginPermission::use_event_bus, ctx.event_bus),
                "has_capability should return true when permission granted and pointer set");
        require(!ctx.has_capability(yuan::plugin::PluginPermission::use_scheduler, ctx.scheduler),
                "has_capability should return false when pointer is null");

        FakeScheduler sched;
        ctx.scheduler = &sched;
        ctx.granted_permissions = yuan::plugin::PluginPermission::use_event_bus;
        require(!ctx.has_capability(yuan::plugin::PluginPermission::use_scheduler, ctx.scheduler),
                "has_capability should return false when pointer is set but permission not granted");
    }

    void test_resource_guard_cleanup_on_stop()
    {
        yuan::app::PluginResourceGuard guard;

        int cleanup_count = 0;

        guard.track("cleanup-test", yuan::plugin::PluginResourceType::event_subscription,
                    [&cleanup_count]() { ++cleanup_count; }, "sub1");
        guard.track("cleanup-test", yuan::plugin::PluginResourceType::scheduler_task,
                    [&cleanup_count]() { ++cleanup_count; }, "task1");
        guard.track("cleanup-test", yuan::plugin::PluginResourceType::callback,
                    [&cleanup_count]() { ++cleanup_count; }, "cb1");

        require(guard.tracked_count("cleanup-test") == 3, "should track 3 resources");

        guard.cleanup_plugin("cleanup-test");
        require(cleanup_count == 3, "all 3 cleanup callbacks should have been called");
        require(guard.tracked_count("cleanup-test") == 0, "no resources should remain after cleanup");
    }

    void test_lifecycle_manager_cleanup_on_stop()
    {
        yuan::app::PluginResourceGuard guard;
        yuan::app::PluginServiceRegistryAdapter registry;

        yuan::plugin::PluginLifecycleManager mgr;
        mgr.set_resource_guard(&guard);
        mgr.set_service_registry(&registry);

        int cleanup_count = 0;
        guard.track("lm-cleanup", yuan::plugin::PluginResourceType::event_subscription,
                    [&cleanup_count]() { ++cleanup_count; }, "sub1");

        auto plugin = new StubPlugin();
        mgr.register_instance("lm-cleanup", plugin, nullptr);
        mgr.transition("lm-cleanup", yuan::plugin::PluginState::initialized);
        mgr.transition("lm-cleanup", yuan::plugin::PluginState::active);

        mgr.stop("lm-cleanup");

        require(mgr.state("lm-cleanup") == yuan::plugin::PluginState::stopped,
                "plugin should be stopped after stop()");
        require(cleanup_count == 1, "resources should be cleaned up when plugin stops");
        require(!guard.has_tracked_resources("lm-cleanup"), "no resources should remain after stop");

        mgr.unload("lm-cleanup");
    }

} // namespace

int main()
{
    std::cout << "=== Phase C: State Machine Tests ===" << std::endl;
    test_state_machine_transitions();
    test_operational_and_callback_states();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Phase C: Fault Injection Tests ===" << std::endl;
    test_call_guard_fault_accumulation();
    test_call_guard_blocks_faulted_plugin();
    test_call_guard_fault_handler();
    test_call_guard_custom_thresholds();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Phase C: Lifecycle Manager Tests ===" << std::endl;
    test_lifecycle_manager_state_transitions();
    test_lifecycle_manager_fault_escalation();
    test_lifecycle_manager_state_change_callback();
    test_lifecycle_manager_cleanup_on_stop();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Phase C: Descriptor Compatibility Tests ===" << std::endl;
    test_manifest_from_meta();
    test_event_descriptors();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Phase C: Capability Enforcement Tests ===" << std::endl;
    test_capability_enforcement();
    test_resource_guard_cleanup_on_stop();
    std::cout << "  PASSED" << std::endl;

    std::cout << "all phase C governance tests passed" << std::endl;
    return 0;
}
