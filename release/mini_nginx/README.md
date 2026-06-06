# mini_nginx (v0)

This is a lightweight nginx-like reverse proxy app built on top of `HttpService` + `HttpProxy`.

## Features in this version

- Load proxy routes from a JSON file
- Prefix-based routing (`root`)
- Upstream balancing (`round_robin`, `random`, `least_conn`, `weighted_rr`)
- Connection pool and timeout knobs
- Optional proxy path rewrite fields (`strip_prefix`, string `rewrite`)
- WebSocket proxy path can be configured as regular routes
- Access log middleware (`ip/method/url`, one line per request)
- Route hot reload (file change polling + `SIGHUP` on non-Windows)
- Windows IOCP listener selection via config/env
- Static-only mode for simple file hosting
- Health endpoint, redirect rules, global response headers, and method allow-list
- nginx-like static cache headers (`expires`, `cache_control`, per-location headers)
- nginx-like static toggles (`autoindex`, `sendfile`, `gzip`, `gzip_static`, `types`, `default_type`)
- nginx-like compression policy controls (`gzip_http_version`, `gzip_disable`, `gzip_proxied`)
- nginx-like proxy request header controls (`proxy_set_header`, `preserve_host`, request-header hiding)
- nginx-like proxy response header controls (`proxy_hide_header`, `proxy_set_response_header`)
- nginx-like in-memory proxy cache (`proxy_cache`, TTL, max response size)
- nginx-like per-location/per-route method limits (`allowed_methods`, `limit_except`)
- nginx-like access rules (`allow`, `deny`, JSON-ordered `access`)
- nginx-like Basic auth (`auth_basic`, per server/location/route, `off` override)
- nginx-like access/auth `satisfy` mode (`all` by default, `any` for allow-list or auth)
- nginx-like return rules (`return`, top-level `returns`)
- nginx-like rewrite rules (`rewrites`, regex capture replacement, internal rewrite or 301/302/303 redirect)
- HTTP/2 h2c and TLS ALPN linkage (`enable_http2`, `http2_tls_only`, `enable_h2c`)
- nginx-like size/time values such as `client_max_body_size: "10m"` and `proxy_read_timeout: "30s"`
- direct `proxy_pass` URLs such as `http://127.0.0.1:9001`

## Build

```powershell
cmake --build build --target mini_nginx -j 4
```

## Run

```powershell
.\build\release\mini_nginx\mini_nginx.exe .\release\mini_nginx\mini_nginx.json
```

Equivalent explicit config flag:

```powershell
.\build\release\mini_nginx\mini_nginx.exe -c .\release\mini_nginx\mini_nginx.json
```

Validate config without starting the listener:

```powershell
.\build\release\mini_nginx\mini_nginx.exe -t -c .\release\mini_nginx\mini_nginx.json
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
  - `server_tokens` bool (adds the default `Server` header when enabled)
  - `server_header` string (default `mini_nginx`)
  - `thread_pool_size` int
  - `worker_processes` int (POSIX: `>1` starts one HTTP listener per worker with `SO_REUSEPORT`)
  - `enable_ssl` bool
  - `ssl_certificate` string
  - `ssl_certificate_key` string
  - `ssl_protocols` array (examples `["TLSv1.2", "TLSv1.3"]`; translated to min/max protocol versions)
  - `ssl_min_version` / `ssl_max_version` string (`TLSv1`, `TLSv1.1`, `TLSv1.2`, `TLSv1.3`)
  - `ssl_ciphers` string for TLS 1.2 and below (OpenSSL cipher list syntax)
  - `ssl_ciphersuites` string for TLS 1.3 suites
  - `ssl_prefer_server_ciphers` bool
  - `enable_http2` bool. With TLS enabled, ALPN advertises `h2` and `http/1.1`
  - `http2_tls_only` bool. When true, cleartext h2c prefaces are rejected and HTTP/2 is accepted only through TLS ALPN
  - `enable_h2c` bool alias; `false` is equivalent to `http2_tls_only: true`
  - `enable_keep_alive` bool
  - `enable_cors` bool
  - `client_max_body_size` size string/int (alias of `max_body_size`, examples `10485760`, `10m`, `1g`)
  - `backlog` int
  - `reuse_addr` bool
  - `reuse_port` bool
  - `exclusive_addr` bool
  - `non_block` bool
  - `use_iocp` bool (Windows IOCP accept/read/write backend)
  - `iocp_worker_count` int
  - `allowed_methods` array (global allow-list, returns 405 when rejected)
  - `allow` / `deny` string or array, or ordered `access` array (global IP access rules)
  - `auth_basic` / `basic_auth` object, string, bool, or `"off"` (global Basic auth)
  - `satisfy` string (`all|any`) combining access and Basic auth. `any` allows a request when either IP access rules pass or Basic auth passes
  - `max_body_size` size string/int
  - `keepalive_timeout` duration string/int (examples `60000`, `60s`, `5m`)
  - `send_timeout` duration string/int (alias of `write_timeout_ms`)
  - `write_timeout_ms` int (static response/body flush timeout)
  - `max_connections` int
  - `max_connections_per_ip` int
  - `max_inflight_requests_per_ip` int
  - `max_concurrent_requests_per_ip` int (alias of `max_inflight_requests_per_ip`)
- `upstreams` object
  - key is upstream name
  - value fields:
    - `balance` (`round_robin|random|least_conn|weighted_rr`)
    - `connect_timeout`, `read_timeout`, `write_timeout` (ms or duration strings)
    - `proxy_connect_timeout`, `proxy_read_timeout`, `proxy_send_timeout` aliases
    - `max_retries`, `pool_size`, `idle_timeout`
    - `failure_threshold`, `unhealthy_cooldown_ms` (passive health circuit-break)
    - `servers` array of `{ "host": "...", "port": 9001, "weight": 1 }`
- `routes` array
  - each route:
    - `path` / `location` prefix, e.g. `/api/`
    - `proxy_pass` upstream name or direct URL (`http://host:port[/prefix]`)
    - optional `match` / `location_match` (`prefix|exact|^~|regex|~|~*|=`), or `exact` bool. Default is `prefix`
    - proxy location priority follows nginx-style ordering: exact `=` first, longest strong prefix `^~` second, regex `~` / `~*` in declaration order third, then longest normal prefix
    - regex routes use the `path` / `location` value as the regex pattern and do not strip prefixes by default
    - optional `return` integer, string body, or object (`code`, `body`/`text`, `url`/`to`, `content_type`)
    - optional `rewrites` / `rewrite_rule` object or array. Internal rewrite targets must start with `/`; redirect codes support `301|302|303`
    - optional `strip_prefix`, `rewrite`
    - optional `preserve_host` / `proxy_preserve_host`
    - optional `proxy_set_header` object. Supported variables: `$host`, `$http_host`, `$remote_addr`, `$proxy_add_x_forwarded_for`, `$scheme`, `$request_uri`, `$uri`
    - optional `hide_request_headers` / `proxy_hide_request_headers` array
    - optional `proxy_hide_header` / `hide_response_headers` array for upstream response headers
    - optional `proxy_set_response_header` / `proxy_response_headers` object to add or replace response headers
    - optional `proxy_redirect` / `proxy_redirects` object or array. Examples: `{ "http://upstream/internal": "/api" }` or `{ "from": "...", "to": "..." }`
    - optional in-memory proxy cache: `proxy_cache` bool, `proxy_cache_valid` / `proxy_cache_ttl` duration, `proxy_cache_max_size` size. Caches configured methods (`GET`/`HEAD` by default) for 200 responses with `Content-Length`; emits `X-Cache: MISS|HIT|BYPASS`
    - optional cache controls: `proxy_cache_methods`, `proxy_cache_key` (`$host`, `$request_uri`, `$uri`, `$route`, `$scheme`), `proxy_cache_bypass_headers`, `proxy_no_cache_headers`, `proxy_cache_ignore_cache_control`, `proxy_cache_ignore_set_cookie`
    - optional `allowed_methods` / `limit_except` array, returns 405 with `Allow` when rejected
    - optional `allow` / `deny` string or array, or ordered `access` array, returns 403 when rejected
    - optional `auth_basic` / `basic_auth` object, string, bool, or `"off"`, returns 401 with `WWW-Authenticate` when rejected

Additional top-level fields:

- `access_log` object
  - `enabled` bool
  - `json` bool (json line format)
  - `path` string
  - `format` string for text logs when `json=false`. Supported variables: `$remote_addr`, `$time_local`, `$request`, `$request_method`, `$request_uri`, `$uri`, `$status`, `$body_bytes_sent`, `$host`, `$http_host`, `$http_referer`, `$http_user_agent`, `$request_time`
- `log_format` string
  - top-level shorthand for `access_log.format`
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
- `error_page` / `error_pages` object
  - static default error-page map applied to every static location unless overridden
  - keys are status codes or space-separated status codes (`"404"`, `"403 404"`)
  - values can be a URI string (`"/404.html"`), an nginx-like `=code URI` string (`"=200 /index.html"`), or an object (`{ "uri": "/404.html", "code": 200 }`)
- `auth_basic` / `basic_auth` object
  - top-level default Basic auth for all requests. `server.auth_basic` is also accepted
  - object form: `{ "realm": "Protected", "users": { "demo": "secret" } }`
  - array user form: `{ "users": [{ "user": "demo", "password": "secret" }] }`
  - htpasswd-style file form: `{ "realm": "Protected", "auth_basic_user_file": "conf/htpasswd" }`
  - string form sets the realm, boolean `false` or string `"off"` disables inherited auth on a more specific route/location
  - supported password forms: plain text, `{PLAIN}password`, and Apache htpasswd `{SHA}` values
  - `$apr1$`, `$1$`, `{SSHA}`, and bcrypt markers are recognized as unsupported and will not authenticate yet
- `satisfy` string
  - top-level shorthand for `server.satisfy`
  - `all` requires both access rules and Basic auth to pass when both are configured
  - `any` allows either a matching access rule or valid Basic credentials
- `returns` array
  - each item: `path` / `location` / `from`
  - `match` / `location_match` (`prefix|exact|^~|=`), or `exact` bool. Default is `exact`
  - `code` integer supported by mini_nginx (common examples `200`, `204`, `301`, `302`, `303`, `404`, `410`)
  - optional `body` / `text` and `content_type` for fixed responses
  - optional `url` / `to` / `location` for redirects (`301|302|303`)
- `rewrites` array
  - each item: `pattern` / `regex` for regex matching, or `from` / `path` / `location` for prefix/exact matching
  - `to` / `replacement` / `target` / `uri` replacement. Regex replacements can use `$1`, `$2`, ...
  - optional `match` / `location_match` (`prefix|exact|regex|~|~*`), `prefix`, `exact`, `case_sensitive`
  - optional `code` / `redirect` (`301|302|303`) for external redirects. Omit it for an internal URI rewrite
  - optional `preserve_query` bool (default `true`) and `preserve_path` bool for prefix rules (default `true`)
- `static` array
  - each item:
    - `location` URL prefix (example `/static`)
    - `match` / `location_match` (`prefix|exact|^~|regex|~|~*|=`), or `exact` bool. Default is `prefix`
    - static location priority follows nginx-style ordering: exact `=` first, longest strong prefix `^~` second, regex `~` / `~*` in declaration order third, then longest normal prefix
    - static regex locations use the full request URI without the leading `/` as the file path under `root`
    - optional `return` integer, string body, or object (`code`, `body`/`text`, `url`/`to`, `content_type`)
    - optional `rewrites` / `rewrite_rule` object or array
    - `root` local directory path
    - `alias` local directory path (same serving semantics as `root`, useful for nginx-style configs)
    - `auto_index` / `autoindex` bool
    - `enable_range` bool
    - `sendfile` bool
    - `gzip` bool (dynamic compression for text assets when zlib/brotli is available)
    - `gzip_static` bool (serve matching `.gz`/`.br` precompressed assets when available)
    - `gzip_min_length` size string/int (examples `1024`, `1k`)
    - `gzip_comp_level` integer `1..9` for dynamic gzip
    - `brotli_comp_level` integer `0..11` for dynamic brotli
    - `gzip_vary` bool (default `true`, controls `Vary: accept-encoding` on compressed responses)
    - `gzip_http_version` string/int (`1.0`, `1.1`, or `2`; default `1.1`)
    - `gzip_disable` regex string or array matched against `User-Agent`; `"off"` disables this filter
    - `gzip_proxied` string or array for requests with `Via`. Supported values: `off`, `any`, `expired`, `no-cache`, `no-store`, `private`, `auth`
    - `gzip_types` array of MIME patterns (examples `text/*`, `application/json`)
    - `allowed_methods` / `limit_except` array, returns 405 with `Allow` when rejected
    - `allow` / `deny` string or array, or ordered `access` array, returns 403 when rejected
    - `default_type` fallback MIME type for unknown extensions
    - `types` nginx-style MIME map, where keys are MIME types and values are extensions/extension arrays
    - `mime_types` extension-to-MIME shorthand object
    - `cache_control` string
    - `expires` integer seconds or duration string (`30s`, `10m`, `1h`, `7d`, `off`)
    - `headers` / `add_headers` object of response headers for this static location
    - `index` array of index file names
    - `try_files` array (example `[$uri, $uri/index.html, /index.html, =404]`)
    - `error_page` / `error_pages` object. Same syntax as top-level, overrides inherited entries
    - `auth_basic` / `basic_auth` object, string, bool, or `"off"`

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
- `error_page` can map static errors to local files while preserving the original status, or use `=code` / object `code` to override the response status.
- `try_files` follows order strictly: if `/index.html` is before `=404`, unknown paths will return index page with 200 (SPA style).

## Access log fields

When `access_log.json=true`, each line is JSON with:

- `ts`, `ip`, `method`, `url`
- `status`
- `upstream`
- `latency_ms`
