# Runtime Worker Placement TODO

This checklist tracks the implementation of `SERVICE_INSTANCE_RUNTIME_REFACTOR.md`.

## Baseline

- [x] Create legacy branch: `codex/legacy-runtime-service-bound`.
- [x] Rewrite architecture document around `RuntimeWorkerPool`, `PlacementPlanner`, and `EndpointManager`.
- [x] Capture pre-refactor benchmark output from `core_runtime_benchmark`.

## Phase 1: Definition and Instance Split

- [x] Add `ServicePlacement` and placement modes.
- [x] Add factory-based `ServiceDefinition`.
- [x] Add worker-local `ServiceInstanceEntry`.
- [x] Keep `ServiceRegistry` as a concrete instance registry.
- [x] Add factory registration API to `Application`.
- [x] Add worker-local instance registration API to `Application`.
- [x] Keep old typed registration as a temporary compatibility wrapper.
- [x] Add tests for factory registration and standalone materialization.

## Phase 2: Runtime Worker Planning

- [x] Add `RuntimeWorkerConfig`.
- [x] Add `WorkerPlan` and `ServiceInstancePlan`.
- [x] Implement placement planner for `singleton`, `all_workers`, `sharded`, `dedicated`, and `disabled`.
- [x] Add planner tests.

## Phase 3: Runtime Identity

- [x] Add explicit runtime worker count and service-instance identity to `RuntimeContext`.
- [x] Propagate worker-plan service identity into worker-local context injection.
- [x] Propagate identity into app events.
- [x] Propagate identity into server service events.
- [x] Propagate identity into plugin context and SDK boundary.

## Phase 4: Endpoint Options

- [x] Split `reuse_addr` and `reuse_port`.
- [x] Add `ListenOptions`.
- [x] Make `reuse_port` failures observable.
- [x] Update listener bind paths.
- [x] Add `EndpointManager` planning model for `WorkerPlan` listener ownership.
- [x] Validate endpoint conflicts before Bootstrap starts factory-based runtime workers.
- [x] Add endpoint manager coverage for private bind, replicated reuse-port bind, internal endpoints, ephemeral ports, and conflicting logical services.

## Phase 5: HTTP Proof

- [x] Move `mini_nginx` HTTP setup into factory/builder construction.
- [x] Add HTTP listen options.
- [x] Support HTTP `all_workers` with explicit `reuse_port` on Linux code path.
- [x] Add Linux integration proof target for multi-worker shared-port HTTP.
- [x] Run Linux shared-port proof on a Linux host/CI runner.
- [x] Avoid private service thread when a worker-owned runtime is provided.
- [x] Add HTTP shared-runtime request regression.
- [x] Add `ServerRuntimeHost::start_inline()` lifecycle regression.

## Phase 6: Worker Execution

- [x] Start in-process `WorkerPlan` workers for `multi_thread` applications registered through `ServiceDefinition`.
- [x] Give each in-process worker its own `NetworkRuntime`/`EventLoop` and inject it through `RuntimeContext::shared_runtime`.
- [x] Add `in_process_worker_runtime` CTest coverage for worker identity, shared runtime injection, dispatch execution, and shutdown.
- [x] Add an HTTP Bootstrap regression proving `HttpService` can run on an in-process worker-owned runtime and serve a real request.
- [x] Add restart/recovery semantics for failed in-process runtime workers.
- [x] Add an in-process worker restart-limit regression that verifies suppressed recovery instead of a busy restart loop.
- [x] Fork one process per `WorkerPlan` on the POSIX supervisor path.
- [x] Let one worker process host multiple service instances.
- [x] Restart the failed `WorkerPlan`, not a service-count-indexed worker.

## Phase 7: Plugin Protocol Services

- [x] Add manifest-level `protocol_services`.
- [x] Add `register_protocol_service` permission.
- [x] Discover plugin protocol services before placement planning.
- [x] Convert plugin protocol service declarations to app `ServiceDescriptor`.
- [x] Initialize plugin runtime inside worker-local service instances.
- [x] Give each `PluginHostService` instance its own `PluginManager` so in-process workers do not overwrite one another's plugin context.
- [x] Add a worker-local plugin protocol service regression that starts two Bootstrap workers and verifies distinct plugin-loaded worker/service-instance identities.
- [x] Bind a concrete plugin protocol service handler to a worker-owned TCP listener and cover it with an echo roundtrip regression.

## Phase 8: Runtime and Release Hardening

- [x] Register `release_filesync_unit` with CTest.
- [x] Expand FileSync coverage for unsafe remote paths, malformed payload encoding, delete/get action accounting, and hash-verified commit semantics.
- [x] Fix FileSync streamed and legacy one-line file apply paths so mismatched payloads remove temp files and do not commit corrupt targets.
- [x] Clean FileSync inbound connection state on close/error callbacks.
- [x] Fix FileSync outbound coroutine scheduling so periodic scans do not manually resume a network-suspended coroutine.
- [x] Keep FileSync polling the peer even when the local manifest is unchanged, so server-side changes still pull in NAT/client-driven mode.
- [x] Add a many-file/many-directory FileSync E2E that covers 1200 files, empty directories, short scan intervals, and server-side changes after initial sync.
- [x] Expand async listener coverage to verify detached coroutine completion and connection object release across concurrent real TCP clients.
- [x] Add a repeatable HTTP request-complete/static-stream tail regression covering keep-alive close, mid-stream abort, and stalled readers closed by write timeout.
- [x] Add a release FileSync Windows E2E script or CTest wrapper around the current manual two-process validation.

## Benchmark

- [x] Build `core_runtime_benchmark`.
- [x] Run benchmark before runtime execution changes.
- [x] Run benchmark after initial worker-plan foundation changes.
- [x] Record both results in a benchmark note.
- [x] Run benchmark after HTTP/reuse-port worker-pool proof.
- [x] Expand `core_runtime_benchmark` to cover timer-backed coroutine lifecycle, persistent echo processing capacity, and concurrent short-connection lifecycle.
- [x] Record expanded benchmark output after full test verification.
- [x] Add a worker-runtime lifecycle benchmark that repeatedly starts/stops Bootstrap-managed in-process workers and verifies init/start/dispatch/stop counts.

## Next TODO

- [x] Run Linux shared-port HTTP proof on a Linux host/CI runner.
- [x] Replace remaining service-local endpoint decisions with `EndpointManager` binding plans.
- [x] Bind concrete plugin protocol handlers to network listeners beyond runtime initialization.
- [x] Add HTTP worker-pool throughput benchmark for `all_workers + reuse_port` on Linux.
- [x] Re-check the HTTP request-complete CPU tail after the runtime worker supervisor path is stable.
- [x] Add an in-process worker circuit-breaker regression once the recovery policy is finalized.

## Completion Verification

Date: 2026-05-17

- Built `test_endpoint_manager`, `test_in_process_worker_runtime`, `test_plugin_governance`, `test_http_shared_reuse_port`, `core_runtime_benchmark`, and `http_worker_pool_benchmark`.
- Full local build passed with `cmake --build build -j 4`.
- Ran the core/runtime/plugin/http regression subset: 25/25 passed.
- Ran build-fix smoke tests for `mini_nginx_config`, `ssh_packet_codec_fuzz`, and `server_runtime_host`: 3/3 passed.
- Ran `core_runtime_benchmark` and `http_worker_pool_benchmark` after the final worker-placement changes.
- HTTP worker-pool re-check still points at request-complete stack overhead as an optimization topic, not a blocker for the runtime-worker placement refactor.
