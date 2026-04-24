# Proxy Tool Guide

Source files live in `test/proxy/` and the executable is built to `cmake-build-mingw/test/proxy/proxy_tool.exe`.

This folder now contains a small proxy toolkit:

- `proxy_tool serve [port]`
- `proxy_tool http-proxy [port]`
- `proxy_tool udp-echo [port]`
- `proxy_tool udp-probe [proxy_host] [proxy_port] [target_host] [target_port] [payload]`

## Ownership Model

The proxy code follows the shared-pointer connection model used by the core runtime:

- `net::ConnectionPtr` is the preferred type for retained connections.
- Accepted and connected sockets are kept alive by the event loop while they are registered.
- `AsyncConnectionContext` owns the connection for the lifetime of the async handler.
- Avoid storing long-lived raw `Connection *` pointers in new proxy code unless you are only observing the connection inside the current callback.

## Build

Build the tool from the `cmake-build-mingw` directory:

```powershell
cmake --build . --target proxy_tool -j 4
```

### CentOS 7 build notes

If you plan to run the proxy on CentOS 7, build it on CentOS 7 or in a CentOS 7-compatible environment whenever possible.

Recommended CMake options for a more self-contained binary:

```bash
cmake -S . -B build-centos7 \
  -DYUAN_BUILD_TESTS=OFF \
  -DYUAN_BUILD_EXAMPLE=OFF \
  -DYUAN_BUILD_SERVERS=OFF \
  -DYUAN_BUILD_LIBS=OFF \
  -DYUAN_BUILD_PLUGINS=OFF \
  -DYUAN_ENABLE_SSH=OFF \
  -DYUAN_PROXY_TOOL_LINK_STATIC_RUNTIME=ON

cmake --build build-centos7 --target proxy_tool -j 4
```

Notes:

- `YUAN_PROXY_TOOL_LINK_STATIC_RUNTIME=ON` only statically links the C++ runtime pieces that are usually safe to bundle.
- It does not force a fully static glibc binary.
- For CentOS 7, that is usually the better tradeoff than a fully static glibc build.

## VS Code

If you open the repository in VS Code, use these flows:

- `Terminal -> Run Task -> build proxy_tool` to compile the proxy helper
- `Run and Debug -> Run Proxy Tool` to start the SOCKS5 server on `1080`
- `Run and Debug -> Run UDP Echo` to start the local UDP echo server
- `Terminal -> Run Task -> udp probe` to validate UDP relay after both are running

Recommended order:

1. Build `proxy_tool`
2. Start `Run Proxy Tool`
3. Start `Run UDP Echo`
4. Run `udp probe`

## Start the SOCKS5 proxy

```powershell
.\test\proxy\proxy_tool.exe serve 1080
```

Defaults:

- `CONNECT` enabled
- `UDP ASSOCIATE` enabled
- authentication disabled

## Verify TCP proxying

Use curl through the SOCKS5 proxy:

```powershell
curl.exe --socks5-hostname 127.0.0.1:1080 -I https://example.com
```

## Verify UDP proxying locally

Start a local UDP echo server:

```powershell
.\test\proxy\proxy_tool.exe udp-echo 19090
```

Then send a UDP packet through the SOCKS5 relay:

```powershell
.\test\proxy\proxy_tool.exe udp-probe 127.0.0.1 1080 127.0.0.1 19090 PING-UDP
```

If everything is working, the probe prints `UDP_OK`.

## Windows system proxy

Set the proxy in:

`Settings` -> `Network & Internet` -> `Proxy`

Use:

- `Address`: `127.0.0.1`
- `Port`: `1080`

Notes:

- Windows system proxy settings are usually HTTP/HTTPS-oriented.
- For SOCKS5-specific apps, configure the app directly if it supports SOCKS5.

## OpenCode through CentOS

OpenCode documents support for the standard proxy environment variables:

- `HTTPS_PROXY`
- `HTTP_PROXY`
- `NO_PROXY`

The important part is the `NO_PROXY` bypass, because OpenCode uses a local HTTP server for its UI/client loop. Always bypass `localhost` and `127.0.0.1`, or the client can route its own local traffic back into the proxy.

Recommended setup for CentOS:

1. Start the HTTP proxy on the CentOS box:

```powershell
./proxy_tool http-proxy 3128
```

2. Make sure the port is reachable from Windows.
3. Point OpenCode at that proxy with `HTTPS_PROXY` and `HTTP_PROXY`.

PowerShell example:

```powershell
$env:HTTPS_PROXY = "http://centos.example.com:3128"
$env:HTTP_PROXY = $env:HTTPS_PROXY
$env:NO_PROXY = "localhost,127.0.0.1"
opencode
```

If the proxy needs basic auth, include it in the URL:

```powershell
$env:HTTPS_PROXY = "http://username:password@centos.example.com:3128"
$env:HTTP_PROXY = $env:HTTPS_PROXY
$env:NO_PROXY = "localhost,127.0.0.1"
opencode
```

If your network uses a custom CA, also set:

```powershell
$env:NODE_EXTRA_CA_CERTS = "D:\certs\corp-ca.pem"
```

Notes:

- OpenCode's documented proxy support is HTTP/HTTPS proxy variables, not a SOCKS-specific mode.
- The `http-proxy` mode in `proxy_tool` is the bridge you want for OpenCode.
- Keep `NO_PROXY=localhost,127.0.0.1` in place so the local OpenCode server stays local.

## HTTPS certificates

There are two common cases:

1. The proxy only tunnels HTTPS with `CONNECT`.
   - In this case the proxy itself does not terminate TLS, so you usually do not need a special certificate for the proxy.
   - `proxy_tool http-proxy` works this way.

2. The company proxy or gateway performs TLS inspection and re-signs certificates.
   - In this case OpenCode must trust the company root CA.
   - OpenCode documents `NODE_EXTRA_CA_CERTS` for custom trust roots.

Typical Windows setup for a company root CA:

```powershell
$env:HTTPS_PROXY = "http://centos.example.com:3128"
$env:HTTP_PROXY = $env:HTTPS_PROXY
$env:NO_PROXY = "localhost,127.0.0.1"
$env:NODE_EXTRA_CA_CERTS = "D:\certs\company-root-ca.pem"
opencode
```

Notes:

- The CA file should be PEM encoded.
- If you use `curl` to test the same path, you can also pass `--cacert` with the same PEM file.
- If the certificate error disappears when you point to a normal public site, the issue is almost always the company root CA, not the proxy tool.

## Verify OpenCode proxying

Before launching OpenCode, you can test the same proxy with curl:

```powershell
curl.exe -x http://centos.example.com:3128 -I https://example.com
```

If that works, OpenCode should be able to use the same `HTTPS_PROXY` / `HTTP_PROXY` settings.

## Browser setup

### Firefox

Firefox supports SOCKS5 directly:

- Open `Settings`
- Search for `Proxy`
- Open `Network Settings`
- Choose `Manual proxy configuration`
- Set `SOCKS Host` to `127.0.0.1`
- Set `Port` to `1080`
- Select `SOCKS v5`
- Enable remote DNS if available

### Chrome / Edge

Chrome and Edge usually follow the system proxy on Windows.

If you want a direct launch flag, use:

```powershell
chrome.exe --proxy-server="socks5://127.0.0.1:1080"
```

For Edge, replace `chrome.exe` with `msedge.exe`.

## Recommended quick check

1. Start `proxy_tool serve 1080`
2. Run the curl TCP check
3. Run `proxy_tool udp-echo 19090`
4. Run `proxy_tool udp-probe 127.0.0.1 1080 127.0.0.1 19090 PING-UDP`

## Production readiness and soak

Use these files under `test/proxy/`:

- `PRODUCTION_READINESS.md` production gating checklist
- `run_proxy_soak.ps1` long-running proxy traffic generator and logger
- `analyze_proxy_soak.ps1` parser for `traffic aggregate 1s` metrics lines

Example:

```powershell
.	estepoxyun_proxy_soak.ps1 -ProxyServerPath .\build\test\proxy\proxy_server.exe -DurationSec 1800 -Concurrency 8
.	estepoxyanalyze_proxy_soak.ps1 -ProxyLog .\logs\proxy_soak\proxy-<timestamp>.log -IdleTailSeconds 30
```

If your paths differ, adjust `-ProxyServerPath` and `-ProxyLog`.

## VS Code notes

- Keep the repository root as the workspace folder so relative paths in tasks resolve correctly.
- If VS Code asks for a build directory, point CMake to `cmake-build-mingw`.
- `curl.exe` is used directly in the terminal because the PowerShell `curl` alias is not the real curl binary.
- If port `1080` is busy, change the launch argument in VS Code or use a different port.
