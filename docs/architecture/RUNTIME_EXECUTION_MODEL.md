# Runtime Execution Model

## Current baseline

- The project currently uses a reactor-style event model.
- The practical runtime unit is still a service-owned `EventLoop`.
- Coroutine helpers assume ownership of an `EventLoop + TimerManager` pair through `RuntimeView`.
- A first scheduler foundation now exists on top of that reactor model: `EventLoop` can accept posted coroutine handles, and `EventLoopScheduler` exposes that as a coroutine-facing scheduler primitive.
- A first scheduler-native awaitable pair now also exists on top of that foundation: coroutine code can now dispatch work into the owning reactor or suspend for a timeout without those new awaitables directly driving `EventLoop::loop()` inline.
- A transition boundary now also exists for top-level callers that still need a blocking edge: `sync_wait(runtime, task)` can drive a scheduler-native inner coroutine chain while keeping the repository's current command-line/test entry style working during migration.
- The task model itself is now also closer to mainstream coroutine runtimes: tasks are lazy-started and only begin execution when explicitly resumed, awaited, or driven by `sync_wait`.
- That task/sync-wait baseline now also covers `void` tasks, so scheduler-native helper chains no longer need placeholder result values just to be driven correctly.
- The older bridge awaitables are no longer exposed through `RuntimeView`, and the dedicated old event-loop bridge awaiter header has been removed, so the main coroutine-facing runtime surface now only points at the scheduler-native direction.
- On the FTP client side, a small condition-variable wait boundary now exists for connection/response/list-output/transfer-idle state, which helps clean up thread-owned client-session workflows that previously relied entirely on `sleep + poll` from callers.
- A small file-presence wait helper now also exists on the FTP client side, which gives download-oriented callers a host-provided waiting edge instead of forcing each caller to spin on the filesystem manually from scratch.
- The first FTP interactive/e2e client-side entrypoints now use that wait boundary directly, which means the cleanup is beginning to propagate from protocol code into caller-facing workflows.
- Connection-event waiting is now beginning to use that same direction too: connection event completion can resume coroutine handles through the owning `EventLoop` instead of requiring the awaitable itself to spin the loop inline.
- Redis is now also beginning to use that transition boundary for non-connect waits, which is important because it proves the new scheduler path can cover both request/response and subscription-style waits instead of only single-shot connect examples.
- BitTorrent's UDP tracker synchronous path now also begins to use that same direction, which matters because it extends the coroutine transition boundary into a second protocol family beyond DNS/HTTP/Redis.
- DNS own-loop synchronous query and BitTorrent UDP tracker handler announce now also use that same transition boundary, which reduces the number of protocol-side paths that still manually spin `EventLoop::loop()` from business logic.
- BitTorrent own-loop startup bootstrap now also avoids one unnecessary self-queued startup hop, which makes the runtime boundary slightly cleaner even though that path is not yet a coroutine entry on its own.
- The dedicated coroutine smoke test is now also fully scheduler-native, which gives the repository a minimal validation point that no longer depends on hand-written `queue_in_loop()` in test code.

## RunMode mapping

### `single_thread`

- Event loop mode: `reactor_per_service`
- Status: implemented
- Coroutine compatibility: yes
- Current behavior:
  - `Application` starts services sequentially
  - each service may own or manage its own reactor/runtime

### `multi_thread`

- Event loop mode: `parallel_service_reactors`
- Status: minimally implemented
- Coroutine compatibility: yes
- Current behavior:
  - `Application` starts services in parallel threads
  - each service still keeps its own reactor/runtime boundary
  - this is not yet a unified repository-wide worker runtime

### `multi_process`

- Event loop mode: `process_reactors`
- Status: minimally scaffolded on POSIX, unsupported on Windows/MinGW
- Coroutine compatibility: yes inside each worker process, but no cross-process coroutine runtime contract exists
- Current behavior:
  - `Bootstrap` acts as a minimal POSIX supervisor
  - each child process hosts one local service/runtime
  - this is intentionally not a same-port prefork accept model
  - worker lifecycle now also emits app-level worker events through `EventBus`
  - supervisor state changes now also emit app-level state events through `EventBus`
  - supervisor can now poll and reap worker exits, and shutdown uses a minimal `TERM -> KILL` sequence
  - abnormal worker exits now trigger a minimal supervisor fail-fast path
  - bounded worker restart is now available for failed workers
  - restart backoff no longer blocks inside the reap path; failed workers now enter a scheduled recovery state until their restart deadline arrives
  - failed-worker restart now also tracks attempts inside a bounded restart window
  - when restart attempts hit the per-window limit, supervisor now suppresses restart until the recovery window rolls over instead of always converting that point into terminal failure
  - worker child processes stay in a bootstrap-owned wait loop after startup, so restarted workers do not depend on entrypoint code continuing to run
  - worker-local startup now reuses the app lifecycle instead of manually calling service init/start forever
  - failed workers may be restarted a bounded number of times
  - supervisor now exposes a minimal state model: `idle / starting / running / recovering / degraded / stopping / stopped`
  - `Bootstrap` now also provides a structured supervisor snapshot for aggregate worker visibility, including separate `recovering_workers` and `suppressed_workers` counts plus a structured reason code for state transitions
  - key service entrypoints now print role, worker identity, and supervisor snapshot at startup
- Missing pieces:
  - more robust process supervisor
  - a richer worker restart policy beyond the current bounded-window suppression baseline
  - process-level signal lifecycle model
  - explicit cross-process runtime coordination policy
  - prefork same-listener worker support

## Prefork boundary

- The current network layer is not prefork-ready.
- Current blockers are:
  - only `SO_REUSEADDR` exists; `SO_REUSEPORT` is not implemented
  - acceptors own their own sockets and do not support inherited/shared listener injection
  - there is no explicit fork-after-listen listener re-registration path for poller/channel state

## Reactor and coroutine fit

- The current coroutine mainline fits `single_thread` well because `RuntimeView` drives a local `EventLoop` and resumes through reactor callbacks or timers.
- The current coroutine mainline also fits the current `multi_thread` model because that mode still runs reactor instances inside service-owned thread boundaries instead of sharing one global scheduler.
- The current coroutine mainline can run inside isolated worker processes because each worker still owns a local `EventLoop + TimerManager`.
- The current scheduler direction should stay reactor-bound:
  coroutine continuations should be posted back to their owning `EventLoop`, not moved to an unrelated global executor.
- What is still missing is the host-side process contract, not the local awaitable primitives.

## Practical judgment

- Today, the correct event loop stance is:
  - keep the repository in reactor mode
  - keep event loops scoped to service/runtime ownership
  - allow `Application` to control startup parallelism, not to centralize all I/O loops yet
- The next milestone for runtime evolution is not "replace reactor".
- The next milestone is "make reactor ownership and coroutine host runtime more explicit across more protocols and services".

## Follow-up plan

For the next refactor pass on coroutine/runtime boundaries, use:

- `docs/NETWORK_RUNTIME_REFACTOR_PLAN.md`

That plan narrows the near-term work to:

- shared async primitive completeness
- datagram and stream client facade extraction
- service runtime host cleanup
- `HttpClient::connect_async(...)` hardening before broader stream-side coroutine adoption
