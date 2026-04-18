#include "plugin_service_registry_adapter.h"
#include "plugin_service_catalog.h"
#include "plugin_resource_guard.h"
#include "application.h"
#include "plugin_host_service.h"
#include "plugin/plugin_context.h"
#include "service.h"
#include "service_registry.h"
#include "eventbus/event_bus.h"

#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{

    void require(bool condition, const std::string &message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            std::exit(1);
        }
    }

    class DummyAppService final : public yuan::app::Service
    {
    public:
        bool init() override
        {
            return true;
        }
        void start() override
        {
        }
        void stop() override
        {
        }
    };

    class FakeEventBus final : public yuan::plugin::HostEventBus
    {
    public:
        yuan::plugin::HostEventSubscription subscribe(const std::string &, yuan::plugin::HostEventHandler) override
        {
            ++subscribe_calls;
            return 42;
        }

        bool unsubscribe(yuan::plugin::HostEventSubscription) override
        {
            ++unsubscribe_calls;
            return true;
        }

        void publish(std::string, std::any) override
        {
            ++publish_calls;
        }

        int subscribe_calls = 0;
        int unsubscribe_calls = 0;
        int publish_calls = 0;
    };

    class FakeScheduler final : public yuan::plugin::HostScheduler
    {
    public:
        yuan::plugin::HostSchedulerTaskId schedule_after(std::chrono::milliseconds,
                                                         yuan::plugin::HostSchedulerCallback,
                                                         const std::string &) override
        {
            ++after_calls;
            return 7;
        }

        yuan::plugin::HostSchedulerTaskId schedule_interval(std::chrono::milliseconds,
                                                            yuan::plugin::HostSchedulerCallback,
                                                            const std::string &) override
        {
            ++interval_calls;
            return 8;
        }

        bool cancel(yuan::plugin::HostSchedulerTaskId) override
        {
            ++cancel_calls;
            return true;
        }

        void cancel_by_prefix(const std::string &) override
        {
            ++cancel_by_prefix_calls;
        }

        bool is_running() const override
        {
            return running;
        }

        int after_calls = 0;
        int interval_calls = 0;
        int cancel_calls = 0;
        int cancel_by_prefix_calls = 0;
        bool running = true;
    };

    class DummyPluginService final : public yuan::plugin::PluginService
    {
    };

    class RecordingPluginService final : public yuan::plugin::PluginService
    {
    public:
        bool init(const yuan::plugin::PluginContext &) override
        {
            ++init_calls;
            return true;
        }

        void start() override
        {
            ++start_calls;
        }

        void stop() override
        {
            ++stop_calls;
        }

        int init_calls = 0;
        int start_calls = 0;
        int stop_calls = 0;
    };

    class ThrowingPluginService final : public yuan::plugin::PluginService
    {
    public:
        bool init(const yuan::plugin::PluginContext &) override
        {
            ++init_calls;
            return true;
        }

        void start() override
        {
            ++start_calls;
            throw std::runtime_error("boom");
        }

        void stop() override
        {
            ++stop_calls;
        }

        int init_calls = 0;
        int start_calls = 0;
        int stop_calls = 0;
    };

    std::filesystem::path find_example_plugins_root()
    {
        auto dir = std::filesystem::current_path();
        for (int i = 0; i < 6; ++i) {
            auto candidate = dir / "plugins" / "examples";
            if (std::filesystem::exists(candidate / "lua_greeter" / "plugin.json") &&
                std::filesystem::exists(candidate / "lua_greeter" / "main.lua") &&
                std::filesystem::exists(candidate / "ts_greeter" / "plugin.json") &&
                std::filesystem::exists(candidate / "ts_greeter" / "main.ts")) {
                return candidate;
            }
            if (!dir.has_parent_path()) {
                break;
            }
            dir = dir.parent_path();
        }
        return {};
    }

    void test_app_service_registry()
    {
        yuan::app::ServiceRegistry registry;
        auto service = std::make_shared<DummyAppService>();

        const bool registered = registry.register_typed_service<DummyAppService>(
            "http",
            service,
            "host.http",
            2);
        if (!registered) {
            std::cerr << "service_valid=" << static_cast<bool>(service)
                      << " name=http version=2\n";
        }
        require(registered,
                "typed host service registration should succeed");

        const auto descriptors = registry.list_descriptors();
        require(descriptors.size() == 1, "host service descriptor list should contain the registered service");
        const auto &descriptor = descriptors.front();
        require(descriptor.contract_id == "host.http", "host service contract id should be preserved");
        require(descriptor.contract_version == 2, "host service contract version should be preserved");
        require(!descriptor.type_name.empty(), "host service type name should be populated");
        require(static_cast<bool>(registry.find_service_as<DummyAppService>("http")),
                "typed host service lookup should resolve the registered type");

        auto shared_registry = std::make_shared<yuan::app::ServiceRegistry>();
        require(shared_registry->register_typed_service<DummyAppService>("http", service, "host.http", 2),
                "shared registry registration should succeed");

        yuan::app::PluginServiceCatalog catalog(shared_registry);
        const auto host_services = catalog.list_services();
        require(host_services.size() == 1,
                "plugin-facing host service catalog should expose registered descriptors");
        const auto &host_descriptor = host_services.front();
        require(host_descriptor.contract_id == "host.http",
                "plugin-facing catalog should use registry contract ids");
        require(host_descriptor.contract_version == 2,
                "plugin-facing catalog should use registry contract versions");
        require(static_cast<bool>(catalog.get_service_as<DummyAppService>("http")),
                "plugin-facing typed host lookup should resolve the registered type");
    }

    void test_application_service_contracts()
    {
        yuan::app::RuntimeContext context;
        context.app_name = "contract-test";

        yuan::app::Application application(context);
        auto service = std::make_shared<DummyAppService>();
        require(application.add_typed_service<DummyAppService>("dns", service, "host.dns", 4),
                "application typed service registration should succeed");
        require(application.services().size() == 1,
                "application should retain one service entry");
        require(application.services().front().descriptor.contract_id == "host.dns",
                "application service entry should preserve contract id");
        require(application.services().front().descriptor.contract_version == 4,
                "application service entry should preserve contract version");
        require(application.context().service_registry != nullptr,
                "application should materialize a service registry on registration");
        require(static_cast<bool>(application.context().service_registry->find_service_as<DummyAppService>("dns")),
                "application service registry should resolve typed services");
    }

    void test_plugin_service_registry_validation()
    {
        yuan::app::PluginServiceRegistryAdapter registry;

        yuan::plugin::PluginServiceDescriptor invalid;
        invalid.name = "broken";
        invalid.contract_id = "plugin.broken";
        invalid.contract_version = 1;
        require(!registry.register_service(
                     "demo",
                     invalid,
                     yuan::plugin::make_plugin_service(std::make_shared<DummyPluginService>())),
                "plugin service registration should reject missing type names");

        yuan::plugin::PluginServiceDescriptor valid;
        valid.name = "demo.echo";
        valid.type_name = "DummyPluginService";
        valid.contract_id = "plugin.demo.echo";
        valid.contract_version = 3;
        valid.plugin_name = "demo";
        require(registry.register_service(
                    "demo",
                    valid,
                    yuan::plugin::make_plugin_service(std::make_shared<DummyPluginService>())),
                "valid plugin service registration should succeed");

        yuan::plugin::PluginServiceDescriptor described;
        require(registry.describe_service("demo.echo", described),
                "registered plugin service descriptor should be queryable");
        require(described.plugin_name == "demo", "plugin service descriptor should record owner");
        require(described.contract_id == "plugin.demo.echo", "plugin service contract id should be preserved");
        require(described.contract_version == 3, "plugin service contract version should be preserved");
    }

    void test_plugin_context_permission_enforcement()
    {
        FakeEventBus event_bus;
        FakeScheduler scheduler;
        yuan::app::PluginServiceRegistryAdapter registry;

        yuan::plugin::PluginContext denied;
        denied.plugin_name = "demo";
        denied.event_bus = &event_bus;
        denied.scheduler = &scheduler;
        denied.service_registry = &registry;
        denied.granted_permissions = yuan::plugin::PluginPermission::use_event_bus;

        yuan::plugin::PluginContextHelper denied_helper(denied);

        require(denied_helper.subscribe_event("event", [](const yuan::plugin::HostEvent &) {}) == 42,
                "allowed event subscription should succeed");
        require(event_bus.subscribe_calls == 1, "event bus should receive allowed subscribe calls");
        require(denied_helper.schedule_task(std::chrono::milliseconds(1), []() {}, "denied") == 0,
                "scheduler helper should reject missing scheduler permission");
        require(scheduler.after_calls == 0, "scheduler should not run when permission is missing");
        require(!denied_helper.register_managed_service<DummyPluginService>(
                     "svc",
                     std::make_shared<DummyPluginService>(),
                     "plugin.demo.svc",
                     1),
                "service registry helper should reject missing registry permission");
        require(!registry.has_service("svc"), "denied plugin service registration should not mutate registry");

        yuan::plugin::PluginContext allowed;
        allowed.plugin_name = "demo";
        allowed.scheduler = &scheduler;
        allowed.service_registry = &registry;
        allowed.granted_permissions =
            yuan::plugin::PluginPermission::use_scheduler |
            yuan::plugin::PluginPermission::use_service_registry;

        yuan::plugin::PluginContextHelper allowed_helper(allowed);

        require(allowed_helper.schedule_interval_task(std::chrono::milliseconds(1), []() {}, "allowed") == 8,
                "scheduler helper should allow granted interval tasks");
        require(scheduler.interval_calls == 1, "scheduler should receive allowed interval calls");
        require(allowed_helper.register_managed_service<DummyPluginService>(
                    "svc.allowed",
                    std::make_shared<DummyPluginService>(),
                    "plugin.demo.allowed",
                    2),
                "service registry helper should allow granted registration");
        require(registry.has_service("svc.allowed"), "allowed plugin service should appear in registry");
    }

    void test_plugin_context_capability_snapshot()
    {
        yuan::plugin::PluginContext ctx;
        ctx.plugin_name = "cap-test";
        ctx.granted_permissions = yuan::plugin::PluginPermission::use_event_bus | yuan::plugin::PluginPermission::use_logger;

        auto caps = ctx.capabilities();
        require(caps.event_bus == false, "capability snapshot should show event_bus unavailable when pointer is null");
        require(caps.logger == false, "capability snapshot should show logger unavailable when pointer is null");
        require(caps.scheduler == false, "capability snapshot should show scheduler unavailable when pointer is null");

        FakeEventBus event_bus;
        ctx.event_bus = &event_bus;
        caps = ctx.capabilities();
        require(caps.event_bus == true, "capability snapshot should show event_bus available when pointer is set");

        require(ctx.can_use(yuan::plugin::PluginPermission::use_event_bus),
                "can_use should return true for granted permission");
        require(!ctx.can_use(yuan::plugin::PluginPermission::use_scheduler),
                "can_use should return false for ungranted permission");

        require(ctx.has_capability(yuan::plugin::PluginPermission::use_event_bus, ctx.event_bus),
                "has_capability should return true when permission granted and pointer set");
        require(!ctx.has_capability(yuan::plugin::PluginPermission::use_scheduler, ctx.scheduler),
                "has_capability should return false when pointer is null");
    }

    void test_plugin_resource_guard_snapshot_and_leak_report()
    {
        yuan::app::PluginResourceGuard guard;

        guard.track("alpha", yuan::plugin::PluginResourceType::event_subscription, []() {}, "sub1");
        guard.track("alpha", yuan::plugin::PluginResourceType::event_subscription, []() {}, "sub2");
        guard.track("alpha", yuan::plugin::PluginResourceType::scheduler_task, []() {}, "task1");

        auto snapshot = guard.resource_snapshot("alpha");
        require(snapshot.count(yuan::plugin::PluginResourceType::event_subscription) > 0,
                "snapshot should contain event_subscription type");
        require(snapshot.at(yuan::plugin::PluginResourceType::event_subscription) == 2,
                "snapshot should count 2 event_subscriptions");
        require(snapshot.count(yuan::plugin::PluginResourceType::scheduler_task) > 0,
                "snapshot should contain scheduler_task type");
        require(snapshot.at(yuan::plugin::PluginResourceType::scheduler_task) == 1,
                "snapshot should count 1 scheduler_task");

        auto report = guard.leak_report("alpha");
        require(!report.empty(), "leak report should not be empty for plugin with tracked resources");
        require(report.find("event_subscription") != std::string::npos,
                "leak report should mention event_subscription");
        require(report.find("scheduler_task") != std::string::npos,
                "leak report should mention scheduler_task");

        auto empty_snapshot = guard.resource_snapshot("nonexistent");
        require(empty_snapshot.empty(), "snapshot for nonexistent plugin should be empty");

        auto empty_report = guard.leak_report("nonexistent");
        require(empty_report.empty(), "leak report for nonexistent plugin should be empty");
    }

    void test_plugin_service_visibility()
    {
        yuan::app::PluginServiceRegistryAdapter registry;

        yuan::plugin::PluginServiceDescriptor public_desc;
        public_desc.name = "pub.svc";
        public_desc.type_name = "DummyPluginService";
        public_desc.contract_id = "plugin.pub";
        public_desc.contract_version = 1;
        public_desc.visibility = yuan::plugin::PluginServiceVisibility::public_;

        yuan::plugin::PluginServiceDescriptor private_desc;
        private_desc.name = "priv.svc";
        private_desc.type_name = "DummyPluginService";
        private_desc.contract_id = "plugin.priv";
        private_desc.contract_version = 1;
        private_desc.visibility = yuan::plugin::PluginServiceVisibility::private_;

        require(registry.register_service("demo", public_desc,
                                          yuan::plugin::make_plugin_service(std::make_shared<DummyPluginService>())),
                "public service registration should succeed");
        require(registry.register_service("demo", private_desc,
                                          yuan::plugin::make_plugin_service(std::make_shared<DummyPluginService>())),
                "private service registration should succeed");

        auto all = registry.list_services();
        require(all.size() == 2, "list_services should return all registered services");

        auto pub_only = registry.list_public_services();
        require(pub_only.size() == 1, "list_public_services should return only public services");
        require(pub_only.front().name == "pub.svc", "public service list should contain the public service");
    }

    void test_plugin_service_start_rollback()
    {
        yuan::app::PluginServiceRegistryAdapter registry;
        yuan::plugin::PluginContext context;
        context.plugin_name = "demo";

        auto ok_service = std::make_shared<RecordingPluginService>();
        auto bad_service = std::make_shared<ThrowingPluginService>();

        yuan::plugin::PluginServiceDescriptor ok_desc;
        ok_desc.name = "demo.ok";
        ok_desc.type_name = "RecordingPluginService";
        ok_desc.contract_id = "plugin.demo.ok";
        ok_desc.contract_version = 1;

        yuan::plugin::PluginServiceDescriptor bad_desc;
        bad_desc.name = "demo.bad";
        bad_desc.type_name = "ThrowingPluginService";
        bad_desc.contract_id = "plugin.demo.bad";
        bad_desc.contract_version = 1;

        require(registry.register_service("demo", ok_desc, yuan::plugin::make_plugin_service(ok_service)),
                "ok plugin service registration should succeed");
        require(registry.register_service("demo", bad_desc, yuan::plugin::make_plugin_service(bad_service)),
                "bad plugin service registration should succeed");

        require(registry.init_plugin_services("demo", context),
                "plugin service init should succeed before start rollback test");
        require(!registry.start_plugin_services("demo"),
                "plugin service start should fail when one service throws");

        require(ok_service->start_calls == 1, "first managed service should have been started once");
        require(ok_service->stop_calls == 1, "first managed service should be rolled back with stop");
        require(bad_service->start_calls == 1, "failing managed service should have been attempted once");
        require(bad_service->stop_calls == 0, "failing managed service should not be stopped during rollback");
    }

    void test_lua_script_plugin_host_smoke()
    {
        const auto plugin_root = find_example_plugins_root();
        require(!plugin_root.empty(), "example plugin root should be discoverable");

        yuan::app::RuntimeContext runtime_context;
        runtime_context.app_name = "lua-smoke";
        runtime_context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::PluginHostService host(plugin_root.string());
        host.set_runtime_context(runtime_context);

        require(host.init(), "plugin host init should register script modules");
        host.start();

        require(host.load_plugin("lua_greeter"), "lua greeter plugin should load successfully");
        require(host.health_check("lua_greeter"), "lua greeter plugin should pass health check");

        host.stop();
    }

    void test_typescript_script_plugin_host_smoke()
    {
        const auto plugin_root = find_example_plugins_root();
        require(!plugin_root.empty(), "example plugin root should be discoverable");

        yuan::app::RuntimeContext runtime_context;
        runtime_context.app_name = "ts-smoke";
        runtime_context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::PluginHostService host(plugin_root.string());
        host.set_runtime_context(runtime_context);

        require(host.init(), "plugin host init should register script modules");
        host.start();

        require(host.load_plugin("ts_greeter"), "typescript greeter plugin should load successfully");
        require(host.health_check("ts_greeter"), "typescript greeter plugin should pass health check");

        host.stop();
    }

} // namespace

int main()
{
    test_app_service_registry();
    test_application_service_contracts();
    test_plugin_service_registry_validation();
    test_plugin_context_permission_enforcement();
    test_plugin_context_capability_snapshot();
    test_plugin_resource_guard_snapshot_and_leak_report();
    test_plugin_service_visibility();
    test_plugin_service_start_rollback();
    test_lua_script_plugin_host_smoke();
    test_typescript_script_plugin_host_smoke();
    std::cout << "plugin contract tests passed\n";
    return 0;
}
