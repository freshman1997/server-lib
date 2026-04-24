# HTTP Module Refactor Plan

This document captures the current HTTP module state, known issues, and the concrete refactor plan so work can continue safely in a new session.

## Current Status

- HTTP server is functional (routing, middleware, static hosting, upload, reverse proxy, websocket handoff).
- Forward proxy and reverse proxy code has been moved under `server/proxy` as a new module.
- Build/test currently pass for core proxy integration:
  - `cmake --build build --target HttpProto ServerProxy ServerServices mini_nginx test_proxy_service_integration -j 4`
  - `ctest --test-dir build -R proxy_service_integration --output-on-failure`

## Critical Architecture Debt (Must Fix First)

1. **CPP include bridge for reverse proxy (high risk)**
   - `protocol/http/src/proxy.cpp` currently includes implementation file:
     - `#include "../../../server/proxy/src/reverse_proxy.cpp"`
   - This is fragile (ODR risk, build boundary ambiguity, duplicate compile semantics).
   - Goal: `HttpProto` should depend on declarations only; implementation should compile only in `ServerProxy`.

2. **Header bridge still tied to migrated type names**
   - `protocol/http/include/proxy.h` currently forwards to new location.
   - Keep compatibility, but reduce coupling by exposing stable API header and removing source-level bridging.

## Performance / Memory Findings

1. `HttpPacket::reset()` does heavy shrink operations per request:
   - `protocol/http/src/packet.cpp` (`buffer_.shrink_to_fit()`, `chunked_checksum_.shrink_to_fit()`, `original_file_name_.shrink_to_fit()`)
   - This increases allocator churn under load.

2. Header lookup is linear scan over map entries:
   - `protocol/http/src/packet.cpp` (`HttpPacket::get_header`)
   - Should use direct hashmap lookup by normalized key.

3. Header insert semantics can keep stale values:
   - `HttpPacket::add_header(const char*, const char*)` uses `emplace`.
   - Should overwrite for duplicate header key in current model.

4. Upload sessions have no TTL cleanup:
   - `protocol/http/src/http_server.cpp` (`uploaded_chunks_` lifecycle)
   - Risk of stale session memory and leftover temp chunks if client disconnects.

5. Reverse proxy connect path may block in connect call path:
   - `server/proxy/src/reverse_proxy.cpp`
   - Needs async/non-blocking connect normalization.

## Feature Completeness Snapshot

- Implemented:
  - HTTP/1.1 request/response pipeline
  - static mounts (`index`, autoindex, `try_files`, `error_page`)
  - middleware (cors/auth/rate-limit/body-limit/connection)
  - upload chunk pipeline
  - reverse proxy + websocket upgrade handoff
- Not fully complete / production-hardening pending:
  - true HTTP/2/HTTP/3 protocol stack
  - fully robust chunked request body path across all server handlers
  - cache validators (`ETag`, `If-Modified-Since`) and compression (`gzip/br`)

## Refactor Roadmap

### Phase P0 (Stability + Boundaries)

1. Remove `cpp` include bridge in `protocol/http/src/proxy.cpp`.
2. Keep only declaration bridge from `protocol/http/include/proxy.h` to stable proxy API header.
3. Ensure reverse proxy implementation is compiled exactly once in `ServerProxy`.

Acceptance:
- Full build passes.
- `proxy_service_integration` passes.
- No duplicate symbol/ODR warnings.

### Phase P1 (Hot Path Performance)

1. Remove per-request aggressive `shrink_to_fit` in `HttpPacket::reset()`.
2. Replace linear header lookup with hashmap `find` by normalized key.
3. Make `add_header(const char*, const char*)` overwrite semantics consistent.

Acceptance:
- No behavior regressions in existing tests.
- Reduced CPU and allocation pressure under basic soak.

### Phase P2 (Upload Resource Governance)

1. Add upload session TTL and periodic cleanup.
2. Cleanup stale `.upload_tmp` chunks by age/upload-id.
3. Add metrics/log lines for cleanup events.

Acceptance:
- Long idle after interrupted uploads does not accumulate stale session entries.

### Phase P3 (Proxy Runtime Hardening)

1. Normalize reverse proxy connect path to async/non-blocking behavior.
2. Add backpressure and timeout observability around upstream dial and pool reuse.
3. Add tests for timeout/half-close/abort with repeated reconnect.

Acceptance:
- Stable under long proxy soak + reconnect storm.

## Static Hosting Notes (Current Behavior)

- `try_files` is order-sensitive.
- With `try_files: ["$uri", "$uri/index.html", "/index.html", "=404"]`, unknown paths return index page (SPA style) before `=404`.
- To force strict 404, remove `/index.html` fallback.

## Next Session Quick Start

Recommended first command batch:

```powershell
cmake --build build --target HttpProto ServerProxy ServerServices mini_nginx test_proxy_service_integration -j 4
ctest --test-dir build -R proxy_service_integration --output-on-failure
```

Then start from **Phase P0** above.
