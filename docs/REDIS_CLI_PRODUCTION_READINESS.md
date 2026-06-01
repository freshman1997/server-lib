# Redis CLI Production Readiness

This note covers the production defaults and operating checks for `libs/redis_cli`.

## Recommended Options

- `connect_timeout_ms_`: set to `1000-5000` for service-to-service traffic. Keep it finite.
- `command_timeout_ms_`: set per workload. Use `50-500ms` for low-latency cache calls and a larger value for blocking Redis commands.
- `timeout_ms_`: fallback timeout only. Prefer explicit connect and command timeouts.
- `reconnect_`: keep `true` for normal request/response clients.
- `max_reconnect_retries_`: use `2-3` for request paths. Use higher values only in background workers.
- `reconnect_delay_ms_`: use `20-100ms` to avoid tight reconnect loops.
- `max_buffered_response_bytes_`: keep a hard cap. The default is `16 MiB`; raise only for known large responses.
- `max_pubsub_pending_messages_`: keep bounded. Size it to the largest burst the subscriber can drain without hiding downstream backpressure.

## Connection Lifecycle

- Redis client runtime is backed by `net::NetworkRuntime`, so poller selection follows core defaults instead of being hardcoded in Redis.
  On Linux this is epoll, on macOS kqueue, and on other platforms the core fallback poller is used.
- `close()` is a user-initiated shutdown. It marks the client closed and disables automatic reconnect for subsequent commands.
- `disconnect()` is an internal or operational disconnect. Subsequent commands may reconnect when `reconnect_` is enabled.
- Command timeout, protocol parse failure, auth failure, and DB select failure use disconnect semantics so the next command can recover when configured.
- After a protocol parse failure, the connection is dropped. Do not reuse a stream after a malformed RESP frame.

## Pool Behavior

- `RedisClientPool::init()` builds clients off to the side and publishes the completed pool atomically.
- `get_round_robin_client()` skips clients that cannot reconnect and returns `nullptr` only when no usable client remains.
- `RedisClientPool::stats()` reports pool size, connected clients, unhealthy clients, and closing state.

## Observability

Use `RedisClient::stats()` for per-client counters:

- `connected`, `closed`, `timeout`
- `in_flight`
- `reconnect_attempts`
- `reconnect_successes`
- `command_timeouts`
- `protocol_errors`

Alert on sustained reconnect attempts, command timeouts, or unhealthy pool count. A single transient reconnect is expected during Redis restarts or network failover.

## Release Gate

Before shipping a change that touches `libs/redis_cli`, run:

```powershell
cmake --build build --target redisCliParsingTest redisCliE2ETest redisCliStressTest -j 8
ctest --test-dir build -R "redis_cli_(parsing|e2e|stress)" --output-on-failure
cmake --build build-mingw-release --target redisCliParsingTest redisCliE2ETest redisCliStressTest -j 8
ctest --test-dir build-mingw-release -R "redis_cli_(parsing|e2e|stress)" --output-on-failure
```

`redis_cli_e2e` and `redis_cli_stress` skip cleanly when Redis is unavailable. For meaningful production validation, run them against a local or test Redis instance.
