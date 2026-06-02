# Core Layer Refactor

## Target Shape

The core modules should follow one-way dependencies:

```text
CoreBase -> Core -> App -> server/protocol/release
```

`Core` owns runtime primitives such as pollers, event loops, sockets, connections,
timers, coroutine awaiters, and platform I/O helpers. `App` owns service
registration, lifecycle orchestration, worker planning, endpoint planning, and
bootstrap/supervisor behavior.

## Current Cleanup

The previous layout kept `native_platform.h` under `core/app/include` while
`core/core/src/net/*` included it directly. To make that compile, `Core`
published `../app/include`, which inverted the intended dependency direction.

This refactor moves native platform helpers into `Core`:

```text
core/core/include/platform/native_platform.h
```

The helpers now live in `yuan::platform`, making their ownership explicit and
removing the need for any `Core` dependency on `App` headers.

## Follow-up Boundaries

The remaining larger cleanup is runtime ownership. `Application` should keep
service composition and lifecycle, while `Bootstrap`/runtime hosts should own
worker threads, worker processes, and `NetworkRuntime` lifetimes. Legacy
service-owned reactors should not add a second ownership path. The unused
legacy TCP/UDP `App` base class has been removed so `core/app` now focuses on
service composition and runtime hosting.
