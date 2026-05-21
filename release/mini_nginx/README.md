# mini_nginx (v0)

This is a lightweight nginx-like reverse proxy app built on top of `HttpService` + `HttpProxy`.

## Features in this version

- Load proxy routes from a JSON file
- Prefix-based routing (`root`)
- Upstream balancing (`round_robin`, `random`, `least_conn`, `weighted_rr`)
- Connection pool and timeout knobs
- Optional path rewrite fields (`strip_prefix`, `rewrite`)
- WebSocket proxy path can be configured as regular routes
- Access log middleware (`ip/method/url`, one line per request)
- Route hot reload (file change polling + `SIGHUP` on non-Windows)
- Windows IOCP listener selection via config/env
- Static-only mode for simple file hosting
- Health endpoint, redirect rules, global response headers, and method allow-list

## Build

```powershell
cmake --build build --target mini_nginx -j 4
```

## Run

```powershell
.\build\release\mini_nginx\mini_nginx.exe .\release\mini_nginx\mini_nginx.json
```

Linux:

```bash
./build/release/mini_nginx/mini_nginx ./release/mini_nginx/mini_nginx.json
```

Or use helper scripts after building:

```bash
bash release/mini_nginx/start.sh
bash release/mini_nginx/stop.sh
```

Health check:

```bash
bash release/mini_nginx/health_check.sh
```

Gate:

```bash
bash release/mini_nginx/gate.sh
```

You can override runtime values with env vars:

- `YUAN_MINI_NGINX_PORT`
- `YUAN_MINI_NGINX_SERVER_NAME`
- `YUAN_MINI_NGINX_WORKERS`
- `YUAN_MINI_NGINX_USE_IOCP`
- `YUAN_MINI_NGINX_ACCESS_LOG`
- `YUAN_MINI_NGINX_ACCESS_LOG_PATH`

Example:

```powershell
$env:YUAN_MINI_NGINX_PORT = "8088"
.\build\release\mini_nginx\mini_nginx.exe
```

## JSON config format (recommended)

Top-level fields:

- `server` object
  - `listen` int
  - `server_name` string
  - `thread_pool_size` int
  - `worker_processes` int (POSIX: `>1` starts one HTTP listener per worker with `SO_REUSEPORT`)
  - `enable_ssl` bool
  - `ssl_certificate` string
  - `ssl_certificate_key` string
  - `enable_keep_alive` bool
  - `enable_cors` bool
  - `backlog` int
  - `reuse_addr` bool
  - `reuse_port` bool
  - `exclusive_addr` bool
  - `non_block` bool
  - `use_iocp` bool (Windows IOCP accept/read/write backend)
  - `iocp_worker_count` int
  - `allowed_methods` array (global allow-list, returns 405 when rejected)
  - `max_body_size` number
  - `write_timeout_ms` int (static response/body flush timeout)
  - `max_connections` int
  - `max_connections_per_ip` int
  - `max_inflight_requests_per_ip` int
  - `max_concurrent_requests_per_ip` int (alias of `max_inflight_requests_per_ip`)
- `upstreams` object
  - key is upstream name
  - value fields:
    - `balance` (`round_robin|random|least_conn|weighted_rr`)
    - `connect_timeout`, `read_timeout`, `write_timeout` (ms)
    - `max_retries`, `pool_size`, `idle_timeout`
    - `failure_threshold`, `unhealthy_cooldown_ms` (passive health circuit-break)
    - `servers` array of `{ "host": "...", "port": 9001, "weight": 1 }`
- `routes` array
  - each route:
    - `path` prefix, e.g. `/api/`
    - `proxy_pass` upstream name
    - optional `strip_prefix`, `rewrite`

Additional top-level fields:

- `access_log` object
  - `enabled` bool
  - `json` bool (json line format)
  - `path` string
- `reload_check_interval_ms` int
- `rate_limit` object
  - `enabled` bool
  - `requests_per_second` int
  - `burst` int
- `health` object
  - `enabled` bool
  - `path` string
  - `json` bool
- `headers` object
  - `add` object of response headers added to every request
- `response_headers` object
  - shorthand object of response headers added to every request
- `redirects` array
  - each item: `from`, `to`, `code` (`301|302|303`), `prefix`, `preserve_path`
- `static` array
  - each item:
    - `location` URL prefix (example `/static`)
    - `root` local directory path
    - `auto_index` bool
    - `enable_range` bool
    - `index` array of index file names
    - `try_files` array (example `[$uri, $uri/index.html, /index.html, =404]`)
    - `error_page` object (example `{ "404": "/404.html" }`)

Example: see `mini_nginx.json`.

## Config compatibility

Only the new structured format is supported. Use `server` plus at least one of:

- `static`
- `upstreams` + `routes`

## Hot reload

- Auto reload: when config file write time changes, routes are reloaded.
- Manual reload on non-Windows: send `SIGHUP` to process.
- Current behavior: route table is reloaded in place; listen port changes require restart.
- With `worker_processes > 1`, route reload currently requires restart because workers own their proxy tables.

## Static hosting behavior

- Directory with index file (`index.html`/`index.htm`) serves that file first.
- Directory without index falls back to auto index listing (HTML by default).
- Add `?json=1` to get JSON directory listing output.
- URL traversal (`..`, backslash escape) is denied.
- `try_files` is supported for nginx-like fallback resolution.
- `error_page.404` can map not-found requests to a local static file.
- `try_files` follows order strictly: if `/index.html` is before `=404`, unknown paths will return index page with 200 (SPA style).

## Access log fields

When `access_log.json=true`, each line is JSON with:

- `ts`, `ip`, `method`, `url`
- `status`
- `upstream`
- `latency_ms`
