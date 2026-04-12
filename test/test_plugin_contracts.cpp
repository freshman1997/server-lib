#include "plugin_service_registry_adapter.h"
#include "plugin_service_catalog.h"
#include "application.h"
#include "plugin/plugin_context.h"
#include "service.h"
#include "service_registry.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
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
    bool init() override { return true; }
    void start() override {}
    void stop() override {}
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

    require(denied.subscribe_event("event", [](const yuan::plugin::HostEvent &) {}) == 42,
            "allowed event subscription should succeed");
    require(event_bus.subscribe_calls == 1, "event bus should receive allowed subscribe calls");
    require(denied.schedule_task(std::chrono::milliseconds(1), []() {}, "denied") == 0,
            "scheduler helper should reject missing scheduler permission");
    require(scheduler.after_calls == 0, "scheduler should not run when permission is missing");
    require(!denied.register_managed_service<DummyPluginService>(
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

    require(allowed.schedule_interval_task(std::chrono::milliseconds(1), []() {}, "allowed") == 8,
            "scheduler helper should allow granted interval tasks");
    require(scheduler.interval_calls == 1, "scheduler should receive allowed interval calls");
    require(allowed.register_managed_service<DummyPluginService>(
                "svc.allowed",
                std::make_shared<DummyPluginService>(),
                "plugin.demo.allowed",
                2),
            "service registry helper should allow granted registration");
    require(registry.has_service("svc.allowed"), "allowed plugin service should appear in registry");
}

} // namespace

int main()
{
    test_app_service_registry();
    test_application_service_contracts();
    test_plugin_service_registry_validation();
    test_plugin_context_permission_enforcement();
    std::cout << "plugin contract tests passed\n";
    return 0;
}
