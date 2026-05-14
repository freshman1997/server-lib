# Service Instance Runtime Refactor

## Purpose

This document defines the refactor needed to remove the current runtime ceiling where `multi_process` effectively means one process per service. The target model is service replication: a logical service can produce multiple runtime instances, and each instance can be placed in a process or thread according to its deployment policy.

This refactor may remove old registration APIs. Compatibility with the current `Application::add_service(...)` style is not required for this phase.

The design also covers how the plugin system should participate in protocol/service extension, so plugins can add protocols or protocol-facing services through stable host contracts instead of reaching into server internals.

## Current Limit

The current multi-process model is service isolation, not service replication.

Relevant files:

- `core/app/include/runtime_context.h`
- `core/app/include/application.h`
- `core/app/src/application.cpp`
- `core/app/include/bootstrap.h`
- `core/app/src/bootstrap.cpp`
- `core/app/src/runtime_plan.cpp`

Current behavior in `Bootstrap::run_multi_process()`:

```cpp
const auto &services = application_.services();
const auto worker_count = services.size();

for (std::size_t i = 0; i < services.size(); ++i) {
    const auto &entry = services[i];
    start_worker_process(entry, i, worker_count, &worker_info);
}
```

That means:

```text
worker process count == service count
```

Current child process behavior in `run_local_service_process(...)` starts only one service entry:

```cpp
local_worker_application_ = std::make_unique<Application>(context);
local_worker_application_->add_service(entry.descriptor, entry.service);
local_worker_application_->start();
```

This prevents a single HTTP/MQTT/SOCKS service from being scaled to multiple worker processes on the same port.

## Target Model

The target model separates logical service definition from runtime instances.

```text
Application
  ServiceDefinition[]
    name
    factory
    deployment

RuntimeSupervisor / Bootstrap
  ServiceInstancePlan[]
    service_name
    service_index
    instance_index
    instance_count
    worker_index
    worker_count

Worker process/thread
  local Application
    one Service instance created from factory
```

Terminology:

- `ServiceDefinition`: logical service registered by the application.
- `ServiceFactory`: callable that creates a fresh service object for each runtime instance.
- `ServiceInstance`: one runtime copy of a logical service.
- `ServiceInstancePlan`: launch-time plan item describing where and how one service instance runs.
- `RuntimeSupervisor`: orchestration role currently implemented by `Bootstrap`.

Example:

```text
Logical services:
  http
  mqtt
  bt

Deployment:
  http x 4 process instances
  mqtt x 2 process instances
  bt   x 1 process instance

Expanded workers:
  worker 0: http instance 0/4
  worker 1: http instance 1/4
  worker 2: http instance 2/4
  worker 3: http instance 3/4
  worker 4: mqtt instance 0/2
  worker 5: mqtt instance 1/2
  worker 6: bt instance 0/1
```

## Non-Goals

- Do not preserve old `Application::add_service(...)` APIs unless they are still useful internally.
- Do not support Windows multi-process in the first phase.
- Do not make every protocol service replicated in the first phase.
- Do not make the event bus cross-process in this refactor.
- Do not allow plugins to directly mutate internal server/protocol objects.
- Do not implement a full out-of-process plugin sandbox in this refactor.

## API Direction

Remove shared-instance service registration as the primary API. A runtime-replicable service must be registered with a factory.

Recommended replacement API:

```cpp
namespace yuan::app
{
    struct ServiceDeployment
    {
        std::size_t process_instances = 1;
        std::size_t thread_instances = 1;
        bool reuse_port = false;
        bool restart_failed_instances = true;
    };

    struct ServiceDescriptor
    {
        std::string name;
        std::string type_name;
        std::string contract_id;
        int contract_version = 1;
        ServiceDeployment deployment;
    };

    using ServiceFactory = std::function<std::shared_ptr<Service>()>;

    struct ServiceDefinition
    {
        ServiceDescriptor descriptor;
        ServiceFactory factory;
    };
}
```

Application registration should become factory-only:

```cpp
bool add_service(ServiceDescriptor descriptor, ServiceFactory factory);
```

Optional convenience wrapper:

```cpp
template <typename T, typename Factory>
bool add_typed_service(ServiceDescriptor descriptor, Factory factory);
```

Example:

```cpp
yuan::app::ServiceDescriptor http;
http.name = "http";
http.contract_id = "yuan.service.http";
http.contract_version = 1;
http.deployment.process_instances = 4;
http.deployment.reuse_port = true;

app.add_service(http, [cfg] {
    return std::make_shared<yuan::server::HttpService>(8080, cfg);
});
```

Required validation:

- `descriptor.name` must not be empty.
- `factory` must not be empty.
- `process_instances == 0` should be normalized to `1` or rejected. Prefer reject for clearer configuration errors.
- `thread_instances == 0` should be normalized to `1` or rejected. Prefer reject.
- Duplicate service names are rejected.
- `process_instances > 1` and `reuse_port == false` should be rejected for services that bind the same endpoint, unless the service declares it does not listen on a shared port.

## Application Refactor

Modify `core/app/include/application.h`.

Replace current `ServiceEntry` with a factory-based definition:

```cpp
struct ServiceDefinition
{
    ServiceDescriptor descriptor;
    ServiceFactory factory;

    std::shared_ptr<Service> create_instance() const
    {
        return factory ? factory() : nullptr;
    }
};
```

`Application` should store definitions, not pre-created service instances, at the top-level supervisor side.

However, a worker-local `Application` still needs to own concrete instances after factory creation. The cleanest split is:

```cpp
struct ServiceDefinition
{
    ServiceDescriptor descriptor;
    ServiceFactory factory;
};

struct ServiceInstanceEntry
{
    ServiceDescriptor descriptor;
    std::shared_ptr<Service> service;
};
```

Recommended `Application` responsibilities:

- Top-level application stores `ServiceDefinition` objects.
- Worker-local application stores `ServiceInstanceEntry` objects.
- `Application::init()` and `Application::start()` operate on concrete instance entries only.
- `Bootstrap` creates the worker-local application and injects concrete instances.

Minimal alternative:

- Keep one vector, but `Application` supports both definitions and already-created instances.
- This is easier to patch but weaker architecturally.

Preferred new methods:

```cpp
bool add_service(ServiceDescriptor descriptor, ServiceFactory factory);
bool add_service_instance(ServiceDescriptor descriptor, std::shared_ptr<Service> service);

const std::vector<ServiceDefinition> &service_definitions() const;
const std::vector<ServiceInstanceEntry> &service_instances() const;
```

`add_service_instance(...)` should be used by `Bootstrap` only when building local worker applications.

## RuntimeContext Refactor

Modify `core/app/include/runtime_context.h`.

Add fields that distinguish global worker identity from service-instance identity:

```cpp
std::string active_service_name;
std::size_t service_index = 0;
std::size_t service_instance_index = 0;
std::size_t service_instance_count = 1;
std::size_t worker_count = 1;
bool listener_reuse_port = false;
```

Keep existing fields unless the implementation can cleanly migrate all references:

```cpp
std::size_t worker_threads = 1;
std::size_t worker_index = 0;
bool is_worker_process = false;
```

Suggested meanings after refactor:

- `worker_index`: global runtime worker index.
- `worker_count`: total expanded runtime workers.
- `worker_threads`: legacy name, either keep as alias for `worker_count` or later rename.
- `service_instance_index`: index within one logical service.
- `service_instance_count`: total instances for the logical service.
- `listener_reuse_port`: worker-local hint for services that bind shared listening ports.

Plugin context should later mirror these fields. See `Plugin Runtime Context` below.

## Instance Plan

Add an internal plan type in `core/app/src/bootstrap.cpp` first. Only move it to a public header if tests need direct access.

```cpp
struct ServiceInstancePlan
{
    std::size_t worker_index = 0;
    std::size_t worker_count = 0;
    std::size_t service_index = 0;
    std::size_t service_instance_index = 0;
    std::size_t service_instance_count = 1;
    const ServiceDefinition *definition = nullptr;
};
```

Build plan helper:

```cpp
std::vector<ServiceInstancePlan> build_service_instance_plan(
    const std::vector<ServiceDefinition> &definitions)
{
    std::size_t total = 0;
    for (const auto &def : definitions) {
        total += def.descriptor.deployment.process_instances;
    }

    std::vector<ServiceInstancePlan> result;
    result.reserve(total);

    std::size_t worker_index = 0;
    for (std::size_t service_index = 0; service_index < definitions.size(); ++service_index) {
        const auto &def = definitions[service_index];
        const auto count = def.descriptor.deployment.process_instances;
        for (std::size_t instance_index = 0; instance_index < count; ++instance_index) {
            result.push_back(ServiceInstancePlan{
                worker_index,
                total,
                service_index,
                instance_index,
                count,
                &def
            });
            ++worker_index;
        }
    }

    return result;
}
```

Validation should happen before this helper, so `process_instances` is already known to be at least `1`.

## Bootstrap Refactor

Modify `core/app/include/bootstrap.h`.

Extend `WorkerProcessInfo`:

```cpp
struct WorkerProcessInfo
{
    std::intptr_t pid = -1;
    std::string service_name;
    std::size_t service_index = 0;
    std::size_t service_instance_index = 0;
    std::size_t service_instance_count = 1;
    std::size_t worker_index = 0;
    std::size_t worker_count = 1;
    ...
};
```

Modify `core/app/src/bootstrap.cpp`.

Current signatures:

```cpp
bool start_worker_process(const ServiceEntry &entry,
                          std::size_t worker_index,
                          std::size_t worker_count,
                          WorkerProcessInfo *worker_info = nullptr);

bool run_local_service_process(const ServiceEntry &entry,
                               std::size_t worker_index,
                               std::size_t worker_count);
```

Replace with instance-plan based signatures:

```cpp
bool start_worker_process(const ServiceInstancePlan &instance,
                          WorkerProcessInfo *worker_info = nullptr);

bool run_local_service_process(const ServiceInstancePlan &instance);
```

`run_multi_process()` should become:

```cpp
const auto plan = build_service_instance_plan(application_.service_definitions());
if (plan.empty()) {
    LOG_WARN("multi-process mode requested without any registered service");
    return false;
}

process_role_ = ProcessRole::supervisor;
set_supervisor_state(SupervisorState::starting, SupervisorReason::spawning_initial_workers);

auto supervisor_context = application_.context();
supervisor_context.worker_count = plan.size();
supervisor_context.worker_threads = plan.size();
supervisor_context.worker_index = 0;
supervisor_context.is_worker_process = false;
application_.set_context(supervisor_context);
ensure_event_bus(application_);

for (const auto &instance : plan) {
    WorkerProcessInfo worker_info;
    fill_worker_info(instance, worker_info);

    if (!start_worker_process(instance, &worker_info)) {
        shutdown_multi_process();
        return false;
    }

    worker_processes_.push_back(std::move(worker_info));
    application_.context().event_bus->publish(
        events::worker_started,
        make_worker_event(application_.context(), worker_processes_.back()));
}
```

`run_local_service_process(...)` should create a fresh service instance:

```cpp
auto service = instance.definition->create_instance();
if (!service) {
    LOG_ERROR("worker {} failed to create service '{}'",
              instance.worker_index,
              instance.definition->descriptor.name);
    return false;
}
```

Then build a worker-local context:

```cpp
auto context = application_.context();
context.worker_threads = instance.worker_count;
context.worker_count = instance.worker_count;
context.worker_index = instance.worker_index;
context.is_worker_process = true;
context.active_service_name = instance.definition->descriptor.name;
context.service_index = instance.service_index;
context.service_instance_index = instance.service_instance_index;
context.service_instance_count = instance.service_instance_count;
context.listener_reuse_port = instance.definition->descriptor.deployment.reuse_port ||
                              instance.service_instance_count > 1;
```

Then start a worker-local app:

```cpp
local_worker_application_ = std::make_unique<Application>(context);
if (!local_worker_application_->add_service_instance(instance.definition->descriptor, service)) {
    LOG_ERROR("worker {} failed to register service '{}'",
              instance.worker_index,
              instance.definition->descriptor.name);
    return false;
}

if (!local_worker_application_->start()) {
    LOG_ERROR("worker {} failed to start service '{}'",
              instance.worker_index,
              instance.definition->descriptor.name);
    return false;
}
```

## Restart Semantics

Restart logic should restart a single service instance, not the whole logical service.

Existing restart fields can remain, but identify the failed unit with:

```text
service_name + service_instance_index
```

Restart lookup should use `worker.service_index` and `worker.service_instance_index` to recreate the same instance plan.

Avoid looking up by only `worker.worker_index` if plan generation can change at runtime. For the first phase the plan can be immutable after startup.

## Runtime Plan

Modify `core/app/src/runtime_plan.cpp` notes.

Current POSIX multi-process note says:

```text
POSIX uses a minimal service-per-process supervisor; each worker keeps a local reactor runtime
```

Replace with:

```text
POSIX uses a service-instance supervisor; each worker runs one expanded service instance with a local reactor runtime
```

Windows remains unsupported in this phase:

```text
multi-process supervisor is not implemented on Windows/MinGW yet
```

## Listener Reuse Port

Multiple process instances of the same listening service need shared-port binding.

Required behavior:

- Linux: set `SO_REUSEADDR` and `SO_REUSEPORT` before bind.
- macOS/BSD: support `SO_REUSEPORT`, but verify semantics separately.
- Windows: do not claim support in this phase.

Recommended new socket/listener type:

```cpp
struct ListenOptions
{
    bool reuse_addr = true;
    bool reuse_port = false;
};
```

Candidate files to inspect and modify:

- `core/core/include/net/socket/*`
- `core/core/src/net/socket/*`
- `core/core/include/net/acceptor/*`
- `core/core/src/net/acceptor/*`
- `core/core/include/net/async/async_listener_host.h`
- `core/core/src/net/async/async_listener_host.cpp`

HTTP first-phase integration:

- Add `bool reuse_port = false;` to `protocol/http/include/http_server.h` in `HttpServerConfig`.
- Pass the config to the listener bind path.
- In `server/services/src/http_service.cpp`, apply `runtime_context_.listener_reuse_port` before `server_->init(...)`.

Example:

```cpp
bool HttpService::init()
{
    if (runtime_context_.listener_reuse_port) {
        config_.reuse_port = true;
    }
    server_ = std::make_unique<yuan::net::http::HttpServer>(config_);
    ...
}
```

If `HttpServer` is constructed before `set_runtime_context(...)`, move construction later or expose a setter before bind. Prefer constructing the concrete server in `init()` after runtime context has been set.

## Service Suitability

Not every service should be replicated by default.

Recommended initial support:

| Service | Initial replication support | Notes |
| --- | --- | --- |
| HTTP | Yes | Needs `SO_REUSEPORT`; stateless handlers work best. |
| SOCKS5 | Later | Usually feasible after listener options are generalized. |
| MQTT | Later | Needs session/subscription state strategy. |
| RTSP | Later | Needs session/media-state strategy. |
| DNS | Later | UDP/TCP listener behavior must be checked. |
| NAS | No | Share metadata, locking, SMB/WebDAV consistency need design. |
| SMB | No | Stateful protocol; not first-phase candidate. |
| BitTorrent | No | DHT/NAT/task state should remain singleton unless redesigned. |
| PluginHostService | Usually No | It controls plugins in one runtime. Protocol plugin instances are separate. |

## Plugin System Impact

The plugin system already has useful boundaries:

- `plugins/core/include/plugin/plugin.h`
- `plugins/core/include/plugin/plugin_context.h`
- `plugins/core/include/plugin/host_service_registry.h`
- `plugins/core/include/plugin/extension_point_registry.h`
- `plugins/host/include/plugin_service_registry_adapter.h`
- `plugins/host/src/plugin_service_registry_adapter.cpp`

Current plugin capabilities include:

- Host service registry for plugin-managed services.
- Extension point registry.
- Host network runtime access for timers and dispatch.
- HTTP interceptor capability.

However, plugin services are currently plugin-local service objects, not first-class app runtime service definitions. That means plugins can register internal managed services, but cannot cleanly add a new protocol-level runtime service that participates in `Bootstrap`, service replication, process supervision, and port binding.

## Plugin Protocol Extension Goal

Plugins should be able to add protocol-facing services through stable host contracts.

Target examples:

```text
Plugin provides a custom protocol service:
  plugin: my_echo_protocol
  service: echo
  listens on: 19000
  process_instances: 2
  reuse_port: true

Plugin provides HTTP route extensions:
  plugin: admin_extra
  routes: /admin/plugin/*
  no new listener

Plugin provides protocol codec/handler extension:
  plugin: mqtt_acl
  extension point: yuan.protocol.mqtt.acl.v1
```

## Plugin Extension Types

Separate plugin extension types into three categories.

### 1. Host Service Extension

Plugin registers a service object inside the plugin host only.

Existing support:

```cpp
HostServiceRegistry::register_managed_service(...)
```

Keep this for plugin-private or plugin-public services that do not need app-level runtime supervision.

### 2. Protocol Extension Point

Plugin contributes behavior to an existing protocol service.

Examples:

- HTTP middleware/interceptor
- MQTT auth/ACL provider
- WebSocket route handler
- NAS policy provider

Existing base:

```cpp
ExtensionPointRegistry
HostHttpInterceptor
```

Recommended contracts:

```text
yuan.protocol.http.interceptor.v1
yuan.protocol.http.route.v1
yuan.protocol.mqtt.auth.v1
yuan.protocol.mqtt.acl.v1
yuan.protocol.websocket.route.v1
```

This type does not create new listening services. It attaches to an existing host service.

### 3. Protocol Service Provider

Plugin creates a new app-level service definition with a factory.

This is the missing piece.

Recommended new host interface:

```cpp
namespace yuan::plugin
{
    struct PluginProtocolServiceDeployment
    {
        std::size_t process_instances = 1;
        std::size_t thread_instances = 1;
        bool reuse_port = false;
    };

    struct PluginProtocolServiceDescriptor
    {
        std::string name;
        std::string protocol;
        std::string contract_id;
        int contract_version = 1;
        int port = 0;
        PluginProtocolServiceDeployment deployment;
    };

    class HostProtocolServiceRegistry
    {
    public:
        virtual ~HostProtocolServiceRegistry() = default;

        virtual bool register_protocol_service(
            const std::string &plugin_name,
            PluginProtocolServiceDescriptor descriptor,
            std::function<std::shared_ptr<app::Service>()> factory) = 0;

        virtual void unregister_plugin_protocol_services(
            const std::string &plugin_name) = 0;
    };
}
```

Important: this should not expose concrete `Application` internals to plugins. The host adapter translates plugin protocol service descriptors into app `ServiceDescriptor` objects.

Because `plugin` core should not necessarily depend on `app::Service`, an ABI-cleaner alternative is to define a plugin-side protocol service abstraction and adapt it:

```cpp
class PluginProtocolService : public PluginService
{
public:
    virtual bool init_protocol(const PluginContext &context) = 0;
    virtual void start_protocol() = 0;
    virtual void stop_protocol() = 0;
};
```

Then the host creates an `app::Service` adapter around the plugin protocol service.

Preferred first-phase implementation:

- Keep native C++ plugin integration simple.
- Add host-side adapter in `plugins/host` that can wrap `std::shared_ptr<plugin::PluginProtocolService>` as `yuan::app::Service`.
- Do not expose all of `app` to script plugins yet.

## Plugin Runtime Context

Modify `plugins/core/include/plugin/plugin_context.h`.

Extend `PluginSdkBoundary` and `PluginContext` to mirror service-instance runtime fields:

```cpp
std::string active_service_name;
std::size_t service_instance_index = 0;
std::size_t service_instance_count = 1;
std::size_t worker_count = 1;
```

Update `PluginContext::sdk_boundary()` to include the fields. Since SDK compatibility is not required right now, bump `host_api_version` from `1` to `2` when this is implemented.

Modify `plugins/host/src/plugin_host_service.cpp` so `make_base_plugin_context(...)` copies these values from `RuntimeContext`.

## Plugin Manifest Additions

Current manifest support already includes extension point concepts. Add protocol service declarations to plugin manifests.

Recommended manifest shape:

```json
{
  "name": "echo_protocol",
  "version": "0.1.0",
  "api_version": 2,
  "permissions": [
    "use_network_runtime",
    "register_protocol_service"
  ],
  "protocol_services": [
    {
      "name": "echo",
      "protocol": "echo",
      "contract_id": "yuan.protocol.echo.service",
      "contract_version": 1,
      "port": 19000,
      "process_instances": 2,
      "reuse_port": true
    }
  ],
  "extension_points": [
    {
      "name": "echo.codec",
      "type": "protocol_codec",
      "contract_id": "yuan.protocol.echo.codec",
      "contract_version": 1
    }
  ]
}
```

Add a new permission:

```text
register_protocol_service
```

This should be separate from `use_service_registry`, because registering a supervised listener is stronger than registering a plugin-local service.

Candidate files:

- `plugins/core/include/plugin/plugin_manifest.h`
- `plugins/core/include/plugin/plugin_permission.h`
- `plugins/core/src/plugin/plugin_permission.cpp`
- `plugins/core/src/plugin/plugin_manager.cpp`
- `plugins/host/src/plugin_host_service.cpp`

## Plugin Protocol Service Lifecycle

Protocol services provided by plugins must have host-managed lifecycle.

Recommended flow:

```text
Plugin discovered
Plugin loaded
Plugin initialized with PluginContext
Plugin registers protocol service provider with HostProtocolServiceRegistry
Application/Bootstrap expands protocol service into ServiceInstancePlan
Worker creates plugin protocol service instance via factory
Worker starts adapted app::Service
```

There are two implementation choices.

### Option A: Startup-Only Plugin Protocol Services

Plugin protocol services are discovered and registered before `Bootstrap::run()` expands the service plan.

Pros:

- Much simpler.
- No dynamic process spawning after startup.
- Fits current `Bootstrap` structure.

Cons:

- Cannot enable a new listening protocol at runtime without restart.

Recommended for the first phase.

### Option B: Dynamic Plugin Protocol Services

Plugin protocol services can be registered after the app is running, and the supervisor can spawn new workers dynamically.

Pros:

- More flexible.

Cons:

- Requires mutable service instance plan.
- Requires runtime worker creation and teardown.
- Requires new supervisor control APIs.

Do not implement this in the first phase.

## Plugin and Multi-Process Caveat

In POSIX `fork()` mode, in-process plugins loaded before fork are copied into workers. This has several risks:

- Plugin state is copied, not shared.
- Timers and resources must be worker-local.
- File descriptors inherited across fork must be controlled.
- Script runtimes may not be fork-safe if active threads exist.

Recommended first-phase rule:

```text
Load protocol-service plugins before worker fork, but create runtime service instances after fork.
Do not start plugin-managed timers/tasks before fork for protocol service workers.
```

If this is too risky, use this stricter rule:

```text
Supervisor reads plugin manifests before fork.
Each worker initializes the plugin runtime locally after fork.
```

The stricter rule is cleaner for long-term design, especially for future Windows spawn support.

## Recommended First-Phase Scope

Implement in this order.

### Phase 1: Factory-only app service model

Files:

- `core/app/include/service_registry.h`
- `core/app/include/application.h`
- `core/app/src/application.cpp`

Changes:

- Add `ServiceDeployment`.
- Replace shared-service registration with factory registration.
- Add worker-local `add_service_instance(...)`.
- Remove or deprecate old `add_service(...)` overloads.

Acceptance:

- App can register services only through factories.
- Worker-local app can start concrete service instances.
- Existing examples are migrated to factory registration.

### Phase 2: Service instance planning

Files:

- `core/app/include/bootstrap.h`
- `core/app/src/bootstrap.cpp`
- `core/app/src/runtime_plan.cpp`

Changes:

- Add `ServiceInstancePlan`.
- Expand `process_instances` into workers.
- Extend `WorkerProcessInfo`.
- Restart one service instance at a time.

Acceptance:

- `http x 4 + mqtt x 2` expands to six workers.
- Logs include `service_instance_index`.
- `process_instances = 1` still behaves like current model except using factories.

### Phase 3: Runtime context injection

Files:

- `core/app/include/runtime_context.h`
- `server/services/src/http_service.cpp`
- `server/services/include/http_service.h`

Changes:

- Add instance identity fields.
- Inject `listener_reuse_port`.
- Ensure service construction can apply context before bind.

Acceptance:

- HTTP service can see its instance index.
- Dashboard and service events can display worker and instance identity.

### Phase 4: Listener reuse port for HTTP

Files to inspect first:

- `core/core/include/net/socket/*`
- `core/core/src/net/socket/*`
- `core/core/include/net/acceptor/*`
- `core/core/src/net/acceptor/*`
- `core/core/include/net/async/async_listener_host.h`
- `core/core/src/net/async/async_listener_host.cpp`
- `protocol/http/include/http_server.h`
- `protocol/http/src/http_server.cpp`

Changes:

- Add listener options.
- Add HTTP `reuse_port` config.
- Apply `SO_REUSEPORT` on Linux.

Acceptance:

- Linux can start two HTTP worker processes on one port.
- Windows compile is not broken, but multi-process remains unsupported.

### Phase 5: Plugin runtime context alignment

Files:

- `plugins/core/include/plugin/plugin_context.h`
- `plugins/host/src/plugin_host_service.cpp`

Changes:

- Add service-instance fields to plugin context.
- Bump plugin host API version to `2`.

Acceptance:

- Plugins can inspect worker and service instance identity.

### Phase 6: Plugin protocol service contract

Files:

- `plugins/core/include/plugin/plugin_permission.h`
- `plugins/core/src/plugin/plugin_permission.cpp`
- `plugins/core/include/plugin/plugin_manifest.h`
- `plugins/core/include/plugin/host_service_registry.h` or a new `host_protocol_service_registry.h`
- `plugins/host/include/plugin_service_registry_adapter.h` or a new adapter
- `plugins/host/src/plugin_service_registry_adapter.cpp` or a new adapter

Changes:

- Add `register_protocol_service` permission.
- Add manifest declarations for `protocol_services`.
- Add host interface for protocol service registration.
- Add adapter from plugin protocol service to `app::Service`.

Acceptance:

- Native plugin can declare/register a protocol service provider.
- The provider is converted into an app `ServiceDefinition` before bootstrap expands the runtime plan.

## Example New Application Setup

```cpp
yuan::app::RuntimeContext context;
context.app_name = "mini_nginx";
context.run_mode = yuan::app::RunMode::multi_process;

yuan::app::Application app(context);

yuan::app::ServiceDescriptor http;
http.name = "http";
http.contract_id = "yuan.service.http";
http.contract_version = 1;
http.deployment.process_instances = 4;
http.deployment.reuse_port = true;

app.add_service(http, [cfg] {
    return std::make_shared<yuan::server::HttpService>(8080, cfg);
});

yuan::app::Bootstrap bootstrap(app);
if (!bootstrap.run()) {
    return 1;
}
```

## Example Plugin Protocol Provider Shape

Native plugin code should eventually be able to do:

```cpp
bool EchoPlugin::on_init(const yuan::plugin::PluginContext &ctx)
{
    if (!ctx.protocol_service_registry) {
        return false;
    }

    yuan::plugin::PluginProtocolServiceDescriptor desc;
    desc.name = "echo";
    desc.protocol = "echo";
    desc.contract_id = "yuan.protocol.echo.service";
    desc.contract_version = 1;
    desc.port = 19000;
    desc.deployment.process_instances = 2;
    desc.deployment.reuse_port = true;

    return ctx.protocol_service_registry->register_protocol_service(
        ctx.plugin_name,
        std::move(desc),
        [] {
            return std::make_shared<EchoPluginProtocolService>();
        });
}
```

If keeping `plugin` core free of `app::Service`, use a host adapter:

```cpp
return ctx.protocol_service_registry->register_protocol_service(
    ctx.plugin_name,
    std::move(desc),
    [] {
        return std::make_shared<EchoPluginProtocolService>();
    });
```

The host adapter wraps `EchoPluginProtocolService` into an `app::Service` implementation before adding it to `Application`.

## Test Plan

Add core tests:

- `test/core/test_service_factory_registration.cpp`
- `test/core/test_service_instance_plan.cpp`
- `test/core/test_bootstrap_worker_identity.cpp`

Test cases:

- Factory registration rejects empty name.
- Factory registration rejects empty factory.
- `process_instances = 0` is rejected.
- One service with one instance expands to one worker.
- One service with four instances expands to four workers.
- Two services with `4 + 2` instances expand to six workers.
- `worker_index` is globally increasing.
- `service_instance_index` resets per service.
- `worker_count` equals total instance count.

HTTP integration tests on Linux:

- HTTP with `process_instances = 2` and `reuse_port = true` starts successfully.
- Requests to the shared port return success.
- Logs show two different worker pids.
- Killing one worker triggers restart for only that service instance.

Plugin tests:

- Plugin context exposes service-instance fields.
- Plugin manifest parser accepts `protocol_services`.
- Permission parser accepts `register_protocol_service`.
- Native plugin can register a protocol service descriptor before bootstrap.

## Migration Notes

Because old APIs can be removed, migrate examples and tests directly to factory registration.

Old style to remove:

```cpp
app.add_service("http", std::make_shared<HttpService>(8080, cfg));
```

New style:

```cpp
yuan::app::ServiceDescriptor desc;
desc.name = "http";
desc.deployment.process_instances = 1;

app.add_service(desc, [cfg] {
    return std::make_shared<HttpService>(8080, cfg);
});
```

Search targets:

```text
add_service(
add_typed_service(
ServiceEntry
services()
run_local_service_process
start_worker_process
```

Likely callers:

- `server/mini_nginx/main.cpp`
- `server/bt_downloader/main.cpp`
- test programs under `test/`
- plugin host integration tests if they create app services

## Risks

1. Reuse-port behavior differs by platform.
2. Plugin runtimes may not be fork-safe if initialized before fork.
3. Process-replicated services cannot share in-memory state.
4. EventBus remains process-local.
5. Admin dashboards may show only worker-local state unless aggregation is added.
6. Stateful protocols need individual replication designs.

## Done Criteria

The refactor is complete when all of these are true:

- App-level service registration is factory-based.
- `Bootstrap` expands `process_instances` into service instance workers.
- Worker identity includes service instance identity.
- Linux HTTP can run multiple worker processes on one port using `SO_REUSEPORT`.
- Existing singleton services can still run with `process_instances = 1`.
- Plugin context exposes runtime instance identity.
- Plugin design has a concrete host contract for protocol service registration, even if only native C++ plugins are supported initially.
