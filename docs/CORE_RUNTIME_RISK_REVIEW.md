# Core Runtime Risk Review

## Scope

This note summarizes design risks and likely bug-prone areas in the current `core` runtime / coroutine / connection stack.

Reviewed areas:

- coroutine task model
- accept / connect / stream I/O awaitables
- async connection wrapper semantics
- runtime / handler ownership interaction

The focus here is not style. It is behavioral risk:

- hangs
- lost events
- handler ownership conflicts
- silent failures
- hard-to-debug integration regressions

## Top Findings

### 1. `async_write` / `async_flush` can miss completion events

Severity: High

Files:

- [core/core/include/coroutine/stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:238)
- [core/core/include/coroutine/stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:427)

Why it is risky:

- `AsyncWriteAwaiter::await_suspend()` calls `connection_->write_and_flush(buffer_)` before installing its proxy handler.
- `AsyncFlushAwaiter::await_suspend()` calls `connection_->flush()` before installing its proxy handler.
- If the connection reaches writable completion or close/error before the proxy handler is installed, the awaiter can miss its only wake-up event.

Likely symptoms:

- coroutine hangs until timeout
- random write stalls under low latency or loopback conditions
- flaky behavior that is hard to reproduce consistently

Why this matters:

- This is a real race, not just a design smell.
- It becomes more visible as more services move from blocking I/O to runtime-managed coroutine I/O.

Recommended direction:

- Install the awaiter proxy before initiating the operation.
- Or better, stop using handler replacement as the completion primitive and move to an independent event subscription / waiter model.

### 2. Awaitables are built on handler replacement, so they are not composition-safe

Severity: High

Files:

- [core/core/include/coroutine/accept_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/accept_awaitable.h:35)
- [core/core/include/coroutine/stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:48)
- [core/core/include/coroutine/stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:169)
- [core/core/include/coroutine/stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:241)
- [core/core/include/coroutine/stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:430)

Why it is risky:

- `async_accept`, `async_read`, `async_write`, and `async_flush` all temporarily replace the active `ConnectionHandler`.
- They later try to restore the previous handler.
- This assumes a single owner of handler state at a time.

Failure modes:

- two concurrent awaiters on one connection overwrite each other
- an upper-layer protocol installs its own handler and gets replaced unexpectedly
- handler restoration returns the wrong owner after nested or interleaved waits
- read/write/close events get delivered to the wrong logical consumer

Why this matters:

- This makes the runtime fragile as protocol stacks become more layered.
- It blocks safe composition of reusable middleware, proxies, tunnels, and monitoring hooks.

Recommended direction:

- Separate business protocol handlers from I/O completion waiters.
- Introduce an event-listener or waiter registry on the connection instead of mutating the primary handler.

### 3. `AsyncConnectionContext` has hidden ownership side effects

Severity: High

Files:

- [core/core/include/net/async/async_connection_context.h](/home/yuan/codes/test/webserver/core/core/include/net/async/async_connection_context.h:27)
- [core/core/include/net/async/async_connection_context.h](/home/yuan/codes/test/webserver/core/core/include/net/async/async_connection_context.h:36)
- [core/core/include/net/async/async_connection_context.h](/home/yuan/codes/test/webserver/core/core/include/net/async/async_connection_context.h:49)

Why it is risky:

- Constructing or move-constructing an `AsyncConnectionContext` immediately calls `conn_->set_connection_handler(default_handler_owner_)`.
- That means it is not a passive wrapper around a connection.
- It mutates control flow ownership as a side effect of object construction.

Likely symptoms:

- upper-layer handler disappears unexpectedly
- protocol state machine stops receiving callbacks after context wrapping
- moving a context changes behavior in ways the caller did not expect

Why this matters:

- This violates the principle of least surprise.
- It makes simple refactors like passing or returning a context object behaviorally dangerous.

Recommended direction:

- Make context construction side-effect free.
- If a default async handler is needed, install it explicitly in a dedicated API.

### 4. Detached coroutine tasks can fail silently

Severity: Medium-High

Files:

- [core/core/include/coroutine/task.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/task.h:200)
- [core/core/include/coroutine/task.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/task.h:263)

Why it is risky:

- `Task<void>::detach()` drops ownership from the wrapper and relies on final suspend to destroy the coroutine.
- If the coroutine throws, the exception is stored in `promise_type::exception_`.
- Detached execution has no mandatory reporting path for that exception.

Likely symptoms:

- background tasks fail with no log
- service behavior degrades without a visible failure signal
- debugging requires deep tracing instead of reading logs

Recommended direction:

- Add a standard detached-task exception sink:
  - logger callback
  - runtime error hook
  - fail-fast mode in debug builds

### 5. `Task<T>::execute()` is not a safe “run to completion” API

Severity: Medium

Files:

- [core/core/include/coroutine/task.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/task.h:123)

Why it is risky:

- `execute()` just calls `resume()` once and returns the current stored value.
- For a coroutine that suspends, this is not a “wait until finished” semantic.
- It also does not call `get_result()`, so exceptions are not rethrown there.

Likely symptoms:

- callers assume they executed the task to completion when they did not
- default-initialized return values leak into business logic
- exception handling is bypassed

Recommended direction:

- Rename or remove `execute()` unless it truly means synchronous completion.
- Force callers to use `co_await`, explicit polling, or a dedicated blocking executor.

### 6. `async_accept` has incomplete failure/close wake-up semantics

Severity: Medium

Files:

- [core/core/include/coroutine/accept_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/accept_awaitable.h:94)
- [core/core/include/coroutine/accept_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/accept_awaitable.h:109)

Why it is risky:

- `AcceptProxyHandler::on_error()` is empty.
- `AcceptProxyHandler::on_close()` is empty.
- The awaiter only resumes on `on_connected()`.

Likely symptoms:

- accept loop does not unwind promptly on shutdown
- listener closure can leave a coroutine suspended longer than expected
- failure handling differs across acceptor implementations

Recommended direction:

- Define explicit accept failure semantics:
  - return `nullptr`
  - or return a richer accept result type
- Resume the waiter on close/error paths, not only on successful accept.

## Architectural Theme

The main structural issue is this:

- `ConnectionHandler` is being used both as:
  - the business protocol callback surface
  - the low-level coroutine wake-up mechanism

That coupling creates fragile ownership rules and makes correctness depend on handler swapping order.

As more services adopt runtime-based async I/O, this design will become a larger source of races and integration bugs.

## Suggested Priority

### Priority 1

- Fix the `async_write` / `async_flush` race.
- Add regression tests for immediate-completion and loopback cases.

### Priority 2

- Remove handler replacement as the primitive used by awaitables.
- Introduce explicit event waiters or connection-local observer lists.

### Priority 3

- Make `AsyncConnectionContext` side-effect free.
- Add explicit APIs for handler ownership changes.

### Priority 4

- Add detached-task exception reporting.
- Tighten `Task<T>` API semantics so synchronous-looking helpers cannot be misused.

## Short Conclusion

The `core` layer is usable, but its coroutine/runtime stack currently has real correctness risk under composition and concurrency.

The most urgent issue is not style or duplication. It is event-loss and ownership races caused by the current handler-swapping async model.
