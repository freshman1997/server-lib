#include "application.h"
#include "app_events.h"
#include "eventbus/event_bus.h"

#include <any>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace
{

class DummyService final : public yuan::app::Service
{
public:
    bool init() override
    {
        ++init_count;
        return true;
    }

    void start() override
    {
        ++start_count;
    }

    void stop() override
    {
        ++stop_count;
    }

    static int init_count;
    static int start_count;
    static int stop_count;
};

int DummyService::init_count = 0;
int DummyService::start_count = 0;
int DummyService::stop_count = 0;

class ContextAwareService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
{
public:
    bool init() override { return true; }
    void start() override {}
    void stop() override {}

    void set_runtime_context(const yuan::app::RuntimeContext &context) override
    {
        last_context = context;
    }

    static yuan::app::RuntimeContext last_context;
};

yuan::app::RuntimeContext ContextAwareService::last_context;

class TypedDummyService final : public yuan::app::Service
{
public:
    bool init() override
    {
        initialized = true;
        return true;
    }

    void start() override
    {
        started = true;
    }

    void stop() override
    {
        stopped = true;
    }

    bool initialized = false;
    bool started = false;
    bool stopped = false;
};

bool require(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main()
{
    yuan::app::Application app;

    yuan::app::ServiceDescriptor descriptor;
    descriptor.name = "dummy";
    descriptor.contract_id = "test.dummy";
    descriptor.contract_version = 1;

    if (!require(app.add_service(descriptor, [] {
            return std::make_shared<DummyService>();
        }), "factory service registration should succeed")) {
        return 1;
    }

    if (!require(app.service_definitions().size() == 1, "application should retain one service definition")) {
        return 1;
    }

    if (!require(app.service_instances().empty(), "factory registration should not create an eager instance")) {
        return 1;
    }

    if (!require(!app.add_service(descriptor, [] {
            return std::make_shared<DummyService>();
        }), "duplicate factory registration should be rejected")) {
        return 1;
    }

    if (!require(app.start(), "standalone application should materialize and start factory services")) {
        return 1;
    }

    if (!require(app.service_instances().size() == 1, "application should materialize one service instance")) {
        return 1;
    }

    if (!require(DummyService::init_count == 1, "materialized service should be initialized once")) {
        return 1;
    }

    if (!require(DummyService::start_count == 1, "materialized service should be started once")) {
        return 1;
    }

    if (!require(app.context().service_registry &&
                 app.context().service_registry->find_service("dummy"),
                 "materialized service should be visible in the instance registry")) {
        return 1;
    }

    app.stop();

    if (!require(DummyService::stop_count == 1, "materialized service should be stopped once")) {
        return 1;
    }

    yuan::app::RuntimeContext context_runtime;
    context_runtime.app_name = "identity-app";
    context_runtime.run_mode = yuan::app::RunMode::multi_thread;
    context_runtime.worker_threads = 8;
    context_runtime.runtime_worker_count = 4;
    context_runtime.runtime_workers.worker_count = 4;
    context_runtime.worker_index = 2;
    context_runtime.is_worker_process = true;
    context_runtime.event_bus = std::make_shared<yuan::eventbus::EventBus>();

    std::optional<yuan::app::ServiceEvent> initialized_event;
    std::optional<yuan::app::ServiceEvent> started_event;
    std::optional<yuan::app::ServiceEvent> stopped_event;
    context_runtime.event_bus->subscribe(yuan::app::events::service_initialized,
        [&](const yuan::eventbus::Event &event) {
            if (const auto *payload = std::any_cast<yuan::app::ServiceEvent>(&event.payload)) {
                initialized_event = *payload;
            }
        });
    context_runtime.event_bus->subscribe(yuan::app::events::service_started,
        [&](const yuan::eventbus::Event &event) {
            if (const auto *payload = std::any_cast<yuan::app::ServiceEvent>(&event.payload)) {
                started_event = *payload;
            }
        });
    context_runtime.event_bus->subscribe(yuan::app::events::service_stopped,
        [&](const yuan::eventbus::Event &event) {
            if (const auto *payload = std::any_cast<yuan::app::ServiceEvent>(&event.payload)) {
                stopped_event = *payload;
            }
        });

    yuan::app::Application context_app(context_runtime);
    yuan::app::ServiceDescriptor context_descriptor;
    context_descriptor.name = "context-aware";
    context_descriptor.contract_id = "test.context-aware";
    context_descriptor.contract_version = 1;

    yuan::app::ServiceInstanceRuntime runtime;
    runtime.service_index = 7;
    runtime.service_instance_index = 2;
    runtime.service_instance_count = 4;
    runtime.listener_reuse_port = true;

    if (!require(context_app.add_service_instance(
            context_descriptor,
            std::make_shared<ContextAwareService>(),
            runtime),
            "explicit service instance runtime registration should succeed")) {
        return 1;
    }

    if (!require(context_app.start(), "context-aware application should start")) {
        return 1;
    }

    if (!require(ContextAwareService::last_context.active_service_name == "context-aware",
                 "context-aware service should receive active service name")) {
        return 1;
    }
    if (!require(ContextAwareService::last_context.service_index == 7 &&
                 ContextAwareService::last_context.service_instance_index == 2 &&
                 ContextAwareService::last_context.service_instance_count == 4,
                 "context-aware service should receive instance identity")) {
        return 1;
    }
    if (!require(ContextAwareService::last_context.listener_reuse_port,
                 "context-aware service should receive listener reuse hint")) {
        return 1;
    }
    if (!require(initialized_event.has_value() && started_event.has_value(),
                 "service lifecycle events should be emitted")) {
        return 1;
    }
    if (!require(initialized_event->app_name == "identity-app" &&
                 initialized_event->worker_threads == 8 &&
                 initialized_event->runtime_worker_count == 4 &&
                 initialized_event->worker_index == 2 &&
                 initialized_event->is_worker_process,
                 "service lifecycle event should receive runtime worker identity")) {
        return 1;
    }
    if (!require(initialized_event->service_name == "context-aware" &&
                 initialized_event->active_service_name == "context-aware" &&
                 initialized_event->service_index == 7 &&
                 initialized_event->service_instance_index == 2 &&
                 initialized_event->service_instance_count == 4 &&
                 initialized_event->listener_reuse_port,
                 "service lifecycle event should receive service-instance identity")) {
        return 1;
    }
    if (!require(started_event->service_instance_index == 2 &&
                 started_event->service_instance_count == 4,
                 "service started event should preserve service-instance identity")) {
        return 1;
    }

    context_app.stop();

    if (!require(stopped_event.has_value() &&
                 stopped_event->service_instance_index == 2 &&
                 stopped_event->service_instance_count == 4,
                 "service stopped event should preserve service-instance identity")) {
        return 1;
    }

    yuan::app::Application typed_app;
    auto typed_service = std::make_shared<TypedDummyService>();
    if (!require(typed_app.add_typed_service<TypedDummyService>(
            "typed",
            typed_service,
            "test.typed",
            2),
            "typed service registration should succeed")) {
        return 1;
    }
    if (!require(!typed_app.add_typed_service<TypedDummyService>(
            "typed",
            std::make_shared<TypedDummyService>(),
            "test.typed.duplicate",
            2),
            "duplicate typed service registration should be rejected")) {
        return 1;
    }

    if (!require(typed_app.start(), "typed service application should start")) {
        return 1;
    }

    auto registry = typed_app.context().service_registry;
    if (!require(static_cast<bool>(registry), "typed service app should have a registry")) {
        return 1;
    }

    if (!require(registry->find_service_as<TypedDummyService>("typed") == typed_service,
                 "typed service should remain discoverable after application init re-registration")) {
        return 1;
    }

    yuan::app::ServiceDescriptor typed_descriptor;
    if (!require(registry->describe_service("typed", typed_descriptor),
                 "typed service descriptor should be describable")) {
        return 1;
    }
    if (!require(typed_descriptor.contract_id == "test.typed" &&
                 typed_descriptor.contract_version == 2,
                 "typed service descriptor should preserve contract metadata")) {
        return 1;
    }
    if (!require(typed_service->initialized && typed_service->started,
                 "typed service should be initialized and started")) {
        return 1;
    }

    typed_app.stop();

    if (!require(typed_service->stopped, "typed service should be stopped")) {
        return 1;
    }
    if (!require(!registry->find_service("typed"),
                 "typed service should be unregistered after application stop")) {
        return 1;
    }

    std::cout << "service definition registration test passed\n";
    return 0;
}
