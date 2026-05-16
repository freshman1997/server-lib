# Runtime Worker Placement Refactor

## Purpose

This document replaces the earlier "one service instance per worker" refactor direction with a
runtime-worker placement model.

The core architecture problem is not only that `multi_process` currently means one worker process per
service. The deeper problem is that the current runtime model couples three independent concepts:

```text
Service        = business capability
Thread/Process = execution resource
Listener/Port  = I/O entry resource
```

The target architecture must separate these concepts.

The scalable model is:

```text
Application declares service capabilities.
RuntimeSupervisor owns execution resources.
EndpointManager owns listening endpoints.
PlacementPlanner maps service instances onto runtime workers.
```

This lets one HTTP service use all CPU cores without requiring more logical services, and it also lets
small singleton services share a worker instead of each consuming a dedicated thread or process.

## Implementation Status

Status as of 2026-05-17:

- `Application` now has a definition catalog (`ServiceDefinition` + `ServiceFactory`) and a separate worker-local instance list.
- `ServiceRegistry` remains the concrete instance registry and is no longer the source of placement truth.
- `RuntimeContext` now carries explicit runtime worker and service-instance identity fields.
- `build_worker_plan()` supports `singleton`, `all_workers`, `sharded`, `dedicated`, and `disabled` placement.
- The `multi_thread` factory-registration path can now start in-process `WorkerPlan` runtime workers, each with its own `NetworkRuntime`/`EventLoop`, and inject that worker runtime into local service instances through `RuntimeContext::shared_runtime`.
- The POSIX `multi_process` path can fork one process per `WorkerPlan`, host multiple service instances in one worker process, and restart the failed worker plan.
- Worker-plan service identity is now injected into worker-local services through `RuntimeContext`.
- Socket/listener binding now uses explicit `ListenOptions`, including separately observable `reuse_addr` and `reuse_port`.
- `EndpointManager` now builds a listener ownership plan from `WorkerPlan`, classifies private binds vs replicated `reuse_port` binds, and rejects conflicting logical services before Bootstrap starts factory-based workers.
- HTTP receives listen options, and `mini_nginx` registers HTTP through a factory/builder path so worker-local instances are configured before startup.
- App events, server service events, plugin events, and the plugin SDK boundary now carry runtime worker and service-instance identity.
- Shared-runtime service startup now has an inline path, so HTTP/WebSocket/SSH/SMB/RTSP/MQTT/SOCKS5/DNS/FTP/Proxy services do not create a private service thread when a worker-owned runtime is provided.
- HTTP shared-runtime request handling is covered by `http_features`; Linux `SO_REUSEPORT` HTTP startup has a dedicated `http_shared_reuse_port` test target that skips on Windows and must still be run on Linux.
- HTTP is also covered through the Bootstrap in-process worker path: a factory-created `HttpService` starts on a worker-owned runtime and serves a real request.
- HTTP static-response tail behavior is now covered by regressions for keep-alive client close, mid-stream abort, and stalled readers closed by `write_timeout_ms` instead of the idle timeout.
- In-process worker failure recovery now restarts the same `WorkerPlan` with backoff/window limits, and restart-limit suppression is covered by regression tests.
- Plugin manifests can declare `protocol_services`; `register_protocol_service` is a first-class permission; `PluginManager` can discover declarations before load; `PluginHost` can convert those declarations to app `ServiceDescriptor`.
- `PluginProtocolServiceAdapter` can register those declarations as app `ServiceDefinition`s and initialize a worker-local plugin runtime per service instance. Each `PluginHostService` owns an isolated `PluginManager`, so two in-process workers can load the same plugin declaration without overwriting one another's context.
- Runtime lifecycle coverage now includes async listener shutdown with concurrent real TCP clients, detached handler coroutine completion, and post-close connection object release checks.
- `release/filesync` now has CTest coverage for remote path safety, action accounting, payload encoding, hash-verified file commit semantics, and a Windows many-tree E2E. The E2E covers 1200 files, empty directories, a scan interval shorter than the transfer duration, and server-side changes after initial sync.
- Windows multi-process remains unsupported in this phase, matching the previous implementation boundary.
- Baseline and current core microbenchmark output is recorded in `RUNTIME_WORKER_PLACEMENT_BENCHMARK.md`; the benchmark now covers runtime dispatch, coroutine lifecycle, timer-backed coroutine lifecycle, TCP connection object lifecycle, persistent echo processing capacity, concurrent short-connection echo lifecycle, and Bootstrap-managed worker lifecycle.

Remaining architectural work is replacing remaining service-local endpoint decisions with `EndpointManager` binding plans, implementing shared-fd/single-owner strategies, running the Linux HTTP shared-port proof on Linux CI/host, binding concrete plugin protocol handlers/listeners beyond runtime initialization, and continuing to retire owned-runtime compatibility paths where a shared worker runtime is available.

## Legacy Limit

The legacy model was service-owned runtime execution.

Relevant files:

- `core/app/include/application.h`
- `core/app/src/application.cpp`
- `core/app/include/bootstrap.h`
- `core/app/src/bootstrap.cpp`
- `core/app/include/runtime_context.h`
- `core/app/include/service_registry.h`
- `server/proxy/src/server_runtime_host.cpp`
- `core/core/include/net/async/async_listener_host.h`
- `core/core/src/net/socket/socket_ops.cpp`

Legacy `Application` state stored already-created concrete services:

```cpp
struct ServiceEntry
{
    ServiceDescriptor descriptor;
    std::shared_ptr<Service> service;
};
```

Legacy `Bootstrap::run_multi_process()` expanded workers from service count:

```cpp
const auto &services = application_.services();
const auto worker_count = services.size();
```

Legacy child process startup injected the parent-side service object:

```cpp
local_worker_application_ = std::make_unique<Application>(context);
local_worker_application_->add_service(entry.descriptor, entry.service);
local_worker_application_->start();
```

Legacy `Application::start_services_multi_thread()` only parallelized service startup and then joined
the startup threads. It does not create a long-running runtime worker pool:

```cpp
for (const auto &entry : services_) {
    workers.emplace_back([service = entry.service] {
        service->start();
    });
}
for (auto &worker : workers) {
    worker.join();
}
```

Some long-running service execution is still service-owned. For example, `ServerRuntimeHost::start()`
creates one thread per service:

```cpp
worker_ = std::thread([fn = std::move(serve_fn), this]() {
    fn();
    started_.store(false);
});
```

This means the performance ceiling is tied to the number of logical services, not to a configured
runtime capacity.

Current migration note: this path remains for compatibility when a service owns its runtime. Services
that receive `RuntimeContext::shared_runtime` now use `ServerRuntimeHost::start_inline()` and rely on
the worker runtime to run the event loop.

There is also an important listener detail: on POSIX, `Socket::set_reuse(true)` currently attempts
`SO_REUSEPORT` implicitly when the platform defines it. That behavior is useful for experiments but
too implicit for a production placement model. Endpoint reuse must become explicit and validated.

## Architecture Goal

The target architecture is a runtime worker pool with explicit service placement.

```text
Application
  ServiceDefinitionCatalog
    http
    mqtt
    nas
    plugin_host

RuntimeSupervisor
  RuntimeWorkerPool
    worker 0: NetworkRuntime/EventLoop/TimerManager
    worker 1: NetworkRuntime/EventLoop/TimerManager
    worker 2: NetworkRuntime/EventLoop/TimerManager
    worker 3: NetworkRuntime/EventLoop/TimerManager

EndpointManager
  endpoint 0.0.0.0:8080/tcp
    binding_strategy: reuse_port | shared_fd | single_owner_dispatch

PlacementPlanner
  worker 0: http[0], metrics[0]
  worker 1: http[1]
  worker 2: http[2]
  worker 3: http[3], mqtt[0]
```

The important change is that a worker can host multiple service instances, and a service can be placed
on multiple workers.

This is more general than the previous target model:

```text
old target:
  ServiceInstancePlan[] -> one service instance per worker

new target:
  WorkerPlan[] -> each worker hosts zero or more service instances
```

## Terminology

- `ServiceDefinition`: a logical service declaration registered by the application.
- `ServiceFactory`: callable that creates a fresh concrete service instance.
- `ServiceInstance`: one runtime copy of a logical service.
- `RuntimeWorker`: one execution slot with an event loop, timer manager, and network runtime.
- `RuntimeWorkerPool`: the configured set of runtime workers.
- `EndpointDefinition`: a declared listening endpoint such as `0.0.0.0:8080/tcp`.
- `EndpointManager`: owner of endpoint validation and bind strategy.
- `PlacementPolicy`: service-level scaling and placement rule.
- `WorkerPlan`: launch-time plan for one runtime worker and its assigned service instances.
- `ServiceInstancePlan`: launch-time plan for one concrete service instance inside a worker.
- `RuntimeSupervisor`: orchestration role currently implemented by `Bootstrap`.

## Non-Goals

- Do not preserve the old shared-instance registration API as the primary API.
- Do not make every protocol service replicated by default.
- Do not make the event bus cross-process in this refactor.
- Do not require Windows multi-process support in the first phase.
- Do not allow plugins to mutate internal app/protocol objects directly.
- Do not implement dynamic plugin-provided listener spawning in the first phase.
- Do not make `SO_REUSEPORT` the only long-term listener strategy.

## Design Principles

1. Execution capacity is configured independently from service count.
2. Service replication is a placement policy, not the worker model itself.
3. Endpoint ownership is explicit and validated before workers start.
4. Top-level application state stores definitions, worker-local state stores instances.
5. Runtime identity must distinguish worker identity from service-instance identity.
6. Stateful protocols are singleton by default unless they declare a state distribution strategy.
7. Plugin protocol services are discovered before planning and instantiated inside workers after fork.

## Runtime Worker Configuration

Add a runtime worker configuration to `RuntimeContext` or a dedicated runtime config object.

Recommended shape:

```cpp
namespace yuan::app
{
    enum class WorkerProcessMode
    {
        in_process,
        process_per_worker,
    };

    struct RuntimeWorkerConfig
    {
        // 0 means auto from the compatibility field during migration.
        std::size_t worker_count = 0;
        WorkerProcessMode process_mode = WorkerProcessMode::in_process;
        bool restart_failed_workers = true;
    };
}
```

For the current repository, `RuntimeContext::worker_threads` should not be reused as the final
worker-count field. Keep it as a compatibility field while introducing an explicit runtime worker
count. The old field can later be renamed or deprecated.

## Placement Policy

Replace raw `process_instances` as the primary scaling knob with a placement policy.

```cpp
namespace yuan::app
{
    enum class PlacementMode
    {
        singleton,     // one instance on one shared worker
        all_workers,   // one instance on every data worker
        sharded,       // N instances spread across workers
        dedicated,     // one or more dedicated workers
        disabled,
    };

    struct ServicePlacement
    {
        PlacementMode mode = PlacementMode::singleton;
        std::size_t instances = 1;
        bool restart_failed_instances = true;
    };
}
```

Suggested initial mapping:

| Service | Initial placement | Notes |
| --- | --- | --- |
| HTTP | `all_workers` | Requires explicit endpoint reuse strategy. |
| WebSocket | `all_workers` or `sharded` later | Connection state is worker-local. |
| SOCKS5 | `all_workers` later | Feasible once listener options are generalized. |
| SSH | `sharded` later | Needs forward/listener state review. |
| FTP | `sharded` later | Active/passive data listeners need endpoint review. |
| MQTT | `singleton` first | Replication needs session/subscription distribution. |
| RTSP | `singleton` first | Session/media state needs placement design. |
| DNS | `sharded` later | UDP/TCP listener semantics must be checked. |
| NAS/SMB | `singleton` | Metadata, locking, and filesystem consistency are stateful. |
| BitTorrent | `singleton` | DHT/NAT/task state should remain singleton unless redesigned. |
| PluginHostService | `singleton` | Plugin runtime should not be casually replicated. |
| Metrics/Admin | `singleton` or shared worker | Avoid per-worker admin duplication at first. |

## Service Definition API

Application registration should become definition-based and factory-based.

```cpp
namespace yuan::app
{
    struct ServiceEndpoint
    {
        std::string name;
        std::string host = "0.0.0.0";
        int port = 0;
        std::string protocol = "tcp";
    };

    struct ServiceDescriptor
    {
        std::string name;
        std::string type_name;
        std::string contract_id;
        int contract_version = 1;
        ServicePlacement placement;
        std::vector<ServiceEndpoint> endpoints;
    };

    using ServiceFactory = std::function<std::shared_ptr<Service>()>;

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
}
```

Preferred application APIs:

```cpp
bool add_service(ServiceDescriptor descriptor, ServiceFactory factory);
bool add_service_instance(ServiceDescriptor descriptor, std::shared_ptr<Service> service);

const std::vector<ServiceDefinition> &service_definitions() const;
const std::vector<ServiceInstanceEntry> &service_instances() const;
```

`add_service_instance(...)` is worker-local and should be used by `Bootstrap` or a worker runtime
builder after a factory creates the concrete service.

Required validation:

- `descriptor.name` must not be empty.
- `descriptor.contract_version` must be positive.
- `factory` must not be empty.
- Duplicate logical service names are rejected.
- `PlacementMode::sharded` requires `instances > 0`.
- `PlacementMode::all_workers` ignores `instances` or treats it as a validation error.
- Listening endpoints must be validated before worker startup.

## Service Definition vs Service Registry

The current `ServiceRegistry` is used as a host-service discovery surface, including plugin-facing
catalogs and HTTP dashboard state. It should remain an instance registry, not become the definition
catalog.

Recommended split:

```text
ServiceDefinitionCatalog
  - logical service definitions
  - factories
  - placement policy
  - endpoint declarations
  - supervisor-side only

ServiceRegistry
  - concrete service instances
  - worker-local lookup
  - plugin host service catalog
  - dashboard/runtime visibility
```

This split prevents plugins and dashboards from confusing "the application declares service X" with
"this worker currently runs instance X[2]".

## Worker Plan

The planner output should be worker-centered, not instance-centered.

```cpp
namespace yuan::app
{
    struct RuntimeIdentity
    {
        std::size_t worker_index = 0;
        std::size_t worker_count = 1;
        bool is_worker_process = false;

        std::string active_service_name;
        std::size_t service_index = 0;
        std::size_t service_instance_index = 0;
        std::size_t service_instance_count = 1;
    };

    struct ServiceInstancePlan
    {
        std::size_t service_index = 0;
        std::size_t service_instance_index = 0;
        std::size_t service_instance_count = 1;
        const ServiceDefinition *definition = nullptr;
    };

    struct WorkerPlan
    {
        std::size_t worker_index = 0;
        std::size_t worker_count = 1;
        bool dedicated = false;
        std::vector<ServiceInstancePlan> service_instances;
    };
}
```

Initial placement behavior:

- `singleton`: put one instance on worker `0`, unless the service requests a dedicated worker.
- `all_workers`: create one instance on every non-dedicated data worker.
- `sharded`: create `instances` service instances and spread them round-robin over workers.
- `dedicated`: allocate dedicated worker plans for that service.
- `disabled`: create no instance.

Example:

```text
runtime.worker_count = 4

http.placement = all_workers
mqtt.placement = sharded(instances = 2)
nas.placement = singleton

WorkerPlan:
  worker 0: http[0/4], mqtt[0/2], nas[0/1]
  worker 1: http[1/4], mqtt[1/2]
  worker 2: http[2/4]
  worker 3: http[3/4]
```

This avoids creating six workers for `http x 4 + mqtt x 2` unless the user explicitly requests
dedicated workers.

## Runtime Context

Extend `RuntimeContext` with explicit runtime identity fields, but keep old fields during migration.

```cpp
struct RuntimeContext
{
    std::string app_name = "webserver";
    RunMode run_mode = RunMode::single_thread;

    // Existing compatibility fields.
    std::size_t worker_threads = 1;
    std::size_t worker_index = 0;
    bool is_worker_process = false;

    // New runtime worker identity.
    std::size_t runtime_worker_count = 1;

    // Active service identity for context injection.
    std::string active_service_name;
    std::size_t service_index = 0;
    std::size_t service_instance_index = 0;
    std::size_t service_instance_count = 1;

    // Endpoint hints for worker-local service init.
    bool listener_reuse_port = false;

    std::shared_ptr<ServiceRegistry> service_registry;
    net::NetworkRuntime *shared_runtime = nullptr;
};
```

Suggested meaning:

- `worker_index`: current runtime worker index.
- `runtime_worker_count`: total workers in the pool.
- `worker_threads`: legacy compatibility field until callers migrate.
- `active_service_name`: the service currently receiving context injection.
- `service_instance_index`: index inside one logical service.
- `service_instance_count`: total instances of that logical service.

Events should mirror the new fields:

- `ApplicationEvent`
- `ServiceEvent`
- `WorkerProcessEvent`
- `ServiceRuntimeEvent`
- plugin lifecycle events
- plugin SDK boundary

## Endpoint Manager

Endpoint binding must be explicit before service instances start.

Recommended endpoint model:

```cpp
namespace yuan::net
{
    enum class EndpointBindingStrategy
    {
        single_owner,
        reuse_port,
        shared_fd,
        single_acceptor_dispatch,
    };

    struct ListenOptions
    {
        bool reuse_addr = true;
        bool reuse_port = false;
        bool exclusive_addr_use = false;
    };
}
```

Current POSIX `set_reuse(true)` behavior should be split into:

```cpp
bool set_reuse_addr(int fd, bool on);
bool set_reuse_port(int fd, bool on);
```

Important requirements:

- `reuse_port` must not be silently best-effort when a replicated endpoint depends on it.
- If `SO_REUSEPORT` is unavailable or fails, planner/startup must fail clearly.
- Windows multi-process should not claim shared-port support in this phase.
- Endpoint declarations should be checked before forking worker processes.

### Binding Strategies

`reuse_port`:

- First implementation target for Linux HTTP replication.
- Each worker binds the same endpoint with `SO_REUSEPORT`.
- Simple but platform-dependent.

`shared_fd`:

- Longer-term POSIX strategy.
- Supervisor binds once, then workers inherit or receive listener fds.
- Cleaner for prefork semantics and avoids platform-specific port distribution differences.

`single_acceptor_dispatch`:

- One acceptor accepts connections and dispatches accepted sockets to workers.
- Useful when `reuse_port` is unavailable.
- More complex because accepted connections must be transferred or assigned safely.

The first phase can implement `reuse_port`, but the architecture should not depend on it as the only
path.

## Service Runtime Ownership

Long term, services should not own long-running threads.

Current pattern:

```text
Service::start()
  -> ServerRuntimeHost::start()
     -> std::thread(...)
```

Target pattern:

```text
RuntimeWorker owns NetworkRuntime/EventLoop.
Service::init() binds resources using the worker runtime.
Service::start() activates handlers/tasks without creating a private thread.
RuntimeWorker runs the event loop.
```

Transition plan:

1. Keep existing service-owned runtime for compatibility.
2. When `RuntimeContext::shared_runtime` is set, services should avoid creating a private runtime
   thread.
3. Split protocol server methods into:
   - bind/init endpoint
   - install handlers
   - run owned runtime only when no shared runtime is provided
4. Move long-running event loop execution to `RuntimeWorker`.

HTTP should be the first service migrated because it is the primary stateless throughput service.

## Bootstrap Refactor

`Bootstrap` should become a supervisor over worker plans.

Recommended high-level flow:

```cpp
bool Bootstrap::run()
{
    auto definitions = application_.service_definitions();
    auto endpoints = endpoint_manager_.collect(definitions);
    if (!endpoint_manager_.validate(endpoints)) {
        return false;
    }

    auto worker_plans = placement_planner_.build(runtime_config_, definitions, endpoints);
    if (worker_plans.empty()) {
        return false;
    }

    if (runtime_config_.process_mode == WorkerProcessMode::process_per_worker) {
        return run_worker_processes(worker_plans);
    }

    return run_in_process_workers(worker_plans);
}
```

Worker-local startup:

```cpp
bool start_worker(const WorkerPlan &worker)
{
    RuntimeWorker runtime(worker.worker_index);
    Application local_app(make_worker_context(worker, runtime));

    for (const auto &instance : worker.service_instances) {
        auto service = instance.definition->factory();
        if (!service) {
            return false;
        }

        auto service_context = make_service_context(worker, instance, runtime);
        if (auto *aware = dynamic_cast<RuntimeContextAwareService *>(&*service)) {
            aware->set_runtime_context(service_context);
        }

        if (!local_app.add_service_instance(instance.definition->descriptor, service)) {
            return false;
        }
    }

    return local_app.start();
}
```

The worker process/thread is the long-running unit. The service instance is not the long-running unit.

## Restart Semantics

Restart should operate at the worker level first, then at the service instance level later.

Phase 1:

- If a worker process crashes, restart the same `WorkerPlan`.
- The replacement worker recreates all service instances assigned to that worker.
- Events identify `worker_index` and all service instances inside the worker.

Later:

- In in-process worker mode, individual service instance restart may be possible.
- For process-per-worker mode, a process crash naturally restarts the whole worker plan.

This is simpler and more honest than trying to restart one service instance inside a failed process.

## HTTP First-Class Migration

HTTP is the first service that should prove the new model.

Current issue:

- `mini_nginx` configures routes and proxy after `bootstrap.run()` by accessing the concrete
  `HttpService` instance.
- In a factory/worker model, the supervisor-side `HttpService` is not the worker-local service
  instance.

Required migration:

- Move HTTP route/static/proxy/middleware setup into a factory-captured builder.
- Worker-created HTTP instances must be fully configured before `init()`.
- Do not rely on post-bootstrap mutation of a supervisor-side `HttpService`.

Example:

```cpp
auto http_builder = [cfg] {
    auto service = std::make_shared<yuan::server::HttpService>(cfg.listen_port, cfg.server_config);
    install_protection_middlewares(*service, cfg);
    install_access_log(*service, cfg);
    install_static_mounts(service->server(), cfg);
    install_routes(service->server().ensure_proxy(), cfg.routes);
    return service;
};

app.add_service(http_descriptor, http_builder);
```

If `server()` is not safe before `init()`, add explicit `HttpService` builder hooks instead of
forcing callers to touch the concrete `HttpServer` too early.

## Plugin System Direction

The plugin protocol service design remains useful, but it should not be first-wave runtime work.

Current plugin loading happens through `PluginHostService::init()`, which is itself an app service.
That is too late for worker planning if plugins are expected to add supervised protocol services.

Recommended direction:

```text
Startup control plane:
  read plugin manifests
  collect protocol service declarations
  validate permissions and endpoints
  add resulting service definitions before placement planning

Worker data plane:
  initialize plugin runtime locally
  create plugin protocol service instances
  run adapted app::Service instances
```

Do not require plugin `on_init()` to dynamically register protocol listeners before the first worker
planning phase. Prefer manifest-first discovery for the initial implementation.

Plugin extension categories remain:

1. Host service extension: plugin-local services managed by `HostServiceRegistry`.
2. Protocol extension point: middleware, route, ACL, codec contributions to existing services.
3. Protocol service provider: new supervised listener service, discovered before planning.

First-phase plugin work should be limited to runtime identity alignment:

- Extend `PluginContext` with worker/service-instance identity.
- Extend `PluginSdkBoundary`.
- Keep plugin host API version changes explicit.

Current implementation note: the control-plane half is now present. Manifests can declare
`protocol_services`, discovery enforces `register_protocol_service`, and PluginHost has an adapter
from plugin protocol declarations to app `ServiceDescriptor`. The remaining data-plane work is an
actual worker-local adapted `Service` that initializes the plugin runtime and binds the declared
protocol implementation.

## Recommended Implementation Phases

### Phase 1: Definition and Instance Split

Files:

- `core/app/include/service_registry.h`
- `core/app/include/application.h`
- `core/app/src/application.cpp`

Changes:

- Add `ServicePlacement`.
- Add `ServiceDefinition` and `ServiceFactory`.
- Add worker-local `ServiceInstanceEntry`.
- Keep old `add_typed_service(...)` temporarily as a compatibility wrapper that creates a singleton
  factory only when safe.
- Keep `ServiceRegistry` as an instance registry.

Acceptance:

- Application can register factory-based service definitions.
- Worker-local applications can register concrete service instances.
- Existing tests still pass through compatibility wrappers.

### Phase 2: Runtime Worker and WorkerPlan

Files:

- `core/app/include/runtime_context.h`
- `core/app/include/bootstrap.h`
- `core/app/src/bootstrap.cpp`
- new planner helper under `core/app/src/`

Changes:

- Add `RuntimeWorkerConfig`.
- Add `WorkerPlan` and `ServiceInstancePlan`.
- Implement planner tests for `singleton`, `all_workers`, `sharded`, and `dedicated`.
- Keep initial runtime execution compatible with current service-owned threads.

Acceptance:

- `runtime.worker_count = 4` and `http all_workers` creates four HTTP service instance plans.
- `mqtt sharded(2)` is placed on two workers without creating two extra workers.
- `nas singleton` is placed once.

### Phase 3: Runtime Identity and Events

Files:

- `core/app/include/runtime_context.h`
- `core/app/include/app_events.h`
- `server/services/include/server_service_events.h`
- `server/proxy/src/server_runtime_host.cpp`
- `plugins/core/include/plugin/plugin_context.h`
- `plugins/host/src/plugin_host_service.cpp`

Changes:

- Add explicit worker count and service-instance identity fields.
- Propagate identity into app events, server service events, and plugin context.
- Keep old fields readable during migration.

Acceptance:

- Dashboard and plugin context can distinguish worker index from service instance index.
- Events include enough identity to debug placement.

### Phase 4: Explicit Endpoint Options

Files:

- `core/app/include/endpoint_manager.h`
- `core/app/src/endpoint_manager.cpp`
- `test/core/test_endpoint_manager.cpp`
- `core/core/include/net/socket/socket_ops.h`
- `core/core/src/net/socket/socket_ops.cpp`
- `core/core/include/net/socket/socket.h`
- `core/core/src/net/socket/socket.cpp`
- `core/core/include/net/async/async_listener_host.h`
- `core/core/include/net/session/stream_server_session.h`
- `core/core/include/net/session/datagram_server_session.h`

Changes:

- Split `reuse_addr` and `reuse_port`.
- Add `ListenOptions`.
- Make `reuse_port` failure observable.
- Stop treating POSIX `SO_REUSEPORT` as an implicit side effect of `set_reuse(true)`.
- Add an `EndpointManager` planning layer that groups `WorkerPlan` endpoints, classifies
  `private_bind` vs replicated `reuse_port`, and reports conflicts between different logical
  services before workers start.

Acceptance:

- Existing singleton listeners still bind.
- Linux HTTP can request `reuse_port` explicitly.
- Windows compile is not broken, but multi-process shared-port support remains unsupported.
- Bootstrap rejects conflicting factory-registered endpoint plans before any worker runtime starts.

### Phase 5: HTTP Placement Proof

Files:

- `protocol/http/include/http_server.h`
- `protocol/http/src/http_server.cpp`
- `server/services/include/http_service.h`
- `server/services/src/http_service.cpp`
- `server/mini_nginx/main.cpp`

Changes:

- Move HTTP setup into factory/builder configuration.
- Add endpoint/listen options to HTTP init path.
- Allow HTTP to run as `all_workers` with explicit `reuse_port` on Linux.
- Ensure `shared_runtime` mode does not require a private service thread.

Acceptance:

- One logical HTTP service can run on four runtime workers.
- Requests to the shared port succeed.
- Logs/events identify different HTTP service instance indices.

Current verification:

- `http_features` covers shared-runtime HTTP request handling on Windows.
- `http_features` also covers Bootstrap-owned in-process worker runtime startup for a real
  `HttpService` request path.
- `http_shared_reuse_port` is a Linux integration target for four HTTP instances sharing one port;
  it intentionally skips on Windows because `SO_REUSEPORT` is not available there.
- `server_runtime_host` covers inline lifecycle and verifies the start function runs on the caller
  thread.

### Phase 6: Worker Execution

Files:

- `core/app/include/bootstrap.h`
- `core/app/src/bootstrap.cpp`

Changes:

- Start one `NetworkRuntime`/`EventLoop` per in-process worker for `multi_thread` factory-registered
  applications.
- Build worker-local applications from `WorkerPlan` and inject `shared_runtime`.
- Fork one process per `WorkerPlan`, not per service.
- A worker process can host multiple service instances.
- Restart recreates the same `WorkerPlan`.

Acceptance:

- `multi_thread + all_workers` creates multiple in-process runtime workers without service-owned
  long-running threads.
- Worker-local services receive `shared_runtime`, worker identity, and service-instance identity.
- In-process worker shutdown stops local services and their event loops.
- `http all_workers + nas singleton` with four workers starts four processes, not five.
- Killing one worker restarts that worker plan.
- Events show worker-level restart with service-instance details.

Current verification:

- `in_process_worker_runtime` covers in-process `WorkerPlan` execution, runtime dispatch, identity,
  listener reuse hints, and shutdown.
- `in_process_worker_runtime` also forces the first worker runtime to exit unexpectedly, then
  verifies `Bootstrap::poll_workers()` restarts the same `WorkerPlan` and returns the supervisor
  snapshot to one healthy running worker.
- The same target covers restart-limit behavior by using an always-failing worker and verifying the
  supervisor reports a suppressed recovery instead of entering a tight restart loop.
- `http_features` covers a real `HttpService` started through Bootstrap on an in-process worker-owned
  runtime.
- `core_runtime_benchmark` now includes `in_process_worker_lifecycle`, which repeatedly starts and
  stops Bootstrap-managed in-process workers and verifies init/start/dispatch/stop lifecycle counts.
- POSIX process-per-worker restart is covered by the existing supervisor tests; Linux shared-port HTTP
  proof still needs a Linux runner.

### Phase 7: Plugin Protocol Services

Files:

- `plugins/core/include/plugin/plugin_manifest.h`
- `plugins/core/include/plugin/plugin_permission.h`
- `plugins/core/src/plugin/plugin_permission.cpp`
- `plugins/core/src/plugin/plugin_manager.cpp`
- `plugins/host/src/plugin_host_service.cpp`
- new protocol service registry/adapter files

Changes:

- Add manifest-level `protocol_services`.
- Add `register_protocol_service` permission.
- Discover protocol service declarations before placement planning.
- Initialize actual plugin runtime inside workers.
- Keep each worker-local plugin host isolated with its own `PluginManager`.

Acceptance:

- A native plugin can declare a protocol service in manifest.
- The service becomes an app `ServiceDefinition` before worker plans are built.
- Worker-local plugin runtime creates the actual adapted service instance.
- Multiple in-process workers can initialize the same plugin declaration without overwriting each
  other's runtime context.

Current verification:

- `plugin_governance` covers permission parsing, manifest-first discovery, missing-permission
  rejection, and conversion to app `ServiceDescriptor`.
- `plugin_governance` also starts a manifest-declared Lua protocol service through
  `PluginProtocolServiceAdapter` on two Bootstrap-managed in-process workers and verifies
  plugin-loaded events carry distinct worker and service-instance indices.
- The remaining plugin protocol work is binding concrete protocol handlers/listeners beyond runtime
  initialization.

## Test Plan

Add unit tests:

- `test/core/test_service_definition_registration.cpp`
- `test/core/test_worker_plan.cpp`
- `test/core/test_runtime_identity.cpp`
- `test/core/test_endpoint_options.cpp`

Planner cases:

- One singleton service on one worker.
- One singleton service with four workers still creates one instance.
- One `all_workers` service with four workers creates four instances.
- `all_workers + singleton` shares workers.
- Two `sharded` services are spread without increasing worker count.
- Dedicated services allocate dedicated workers.
- Duplicate service names are rejected.
- Invalid endpoint reuse strategy is rejected.

HTTP integration tests on Linux:

- HTTP `all_workers` with `reuse_port` starts successfully.
- Requests to the shared port return success.
- Logs/events show multiple worker indices and service instance indices.
- Killing one worker process restarts that worker plan.

Plugin tests:

- Plugin context exposes runtime worker and service-instance identity.
- Manifest parser accepts `protocol_services`.
- Permission parser accepts `register_protocol_service`.
- Worker-local `PluginProtocolServiceAdapter` initializes one plugin runtime per planned service
  instance and preserves worker/service-instance identity in plugin events.

## Migration Notes

Old style:

```cpp
auto service = std::make_shared<HttpService>(8080, cfg);
app.add_typed_service<HttpService>("http", service, "server.http", 1);
```

Transitional style:

```cpp
yuan::app::ServiceDescriptor desc;
desc.name = "http";
desc.contract_id = "server.http";
desc.contract_version = 1;
desc.placement.mode = yuan::app::PlacementMode::singleton;

app.add_service(desc, [cfg] {
    return std::make_shared<HttpService>(8080, cfg);
});
```

Scalable HTTP style:

```cpp
yuan::app::ServiceDescriptor desc;
desc.name = "http";
desc.contract_id = "server.http";
desc.contract_version = 1;
desc.placement.mode = yuan::app::PlacementMode::all_workers;
desc.endpoints.push_back({"http", "0.0.0.0", 8080, "tcp"});

app.add_service(desc, [cfg] {
    return make_configured_http_service(cfg);
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
ServerRuntimeHost::start
set_reuse(
listener_.bind(
```

Likely callers:

- `main.cpp`
- `server/mini_nginx/main.cpp`
- `server/bt_downloader/main.cpp`
- `server/match/main.cpp`
- `test/protocol/http/test_http_server.cpp`
- `test/protocol/websocket/test_websocket_server.cpp`
- `test/protocol/ftp/test_ftp_server.cpp`
- plugin host integration tests

## Risks

1. Existing services may assume they own their runtime thread.
2. HTTP setup currently mutates the concrete service after registration.
3. `SO_REUSEPORT` behavior differs by platform and must be explicit.
4. Worker-level restart restarts all instances assigned to that worker.
5. EventBus remains process-local.
6. Dashboard state may be worker-local until aggregation exists.
7. Stateful protocols need separate state distribution designs.
8. Plugin runtimes may not be fork-safe if initialized before worker fork.

## Done Criteria

The refactor is complete when all of these are true:

- App-level services are registered as definitions with factories.
- Execution capacity is configured through runtime workers, not service count.
- `WorkerPlan` can place multiple service instances on one worker.
- One logical HTTP service can run across multiple runtime workers.
- Endpoint reuse is explicit and validated.
- Worker and service-instance identity are visible in context and events.
- Singleton services still run with one instance.
- POSIX process mode forks one worker per `WorkerPlan`.
- Plugin runtime context exposes placement identity.
- Plugin protocol services have a manifest-first path into app service definitions.
