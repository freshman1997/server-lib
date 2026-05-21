# release/proxy

This directory contains release-oriented runnable assets for proxy service and proxy utility tool.

## Binaries

- `release_proxy_server`: HTTP proxy service entry with optional embedded SOCKS5 service
- `release_proxy_tool`: utility tool for SOCKS5 serve/http-proxy/udp-echo/udp-probe

Default build output:

- `build/release/proxy/release_proxy_server`
- `build/release/proxy/release_proxy_tool`

## Build

```bash
cmake --build build --target release_proxy_tool release_proxy_server -j 4
```

For a CentOS 7 style build:

```bash
cmake -S . -B build-centos7 \
  -DYUAN_BUILD_TESTS=OFF \
  -DYUAN_BUILD_EXAMPLE=OFF \
  -DYUAN_BUILD_SERVERS=OFF \
  -DYUAN_BUILD_LIBS=OFF \
  -DYUAN_BUILD_PLUGINS=OFF \
  -DYUAN_ENABLE_SSH=OFF \
  -DYUAN_PROXY_TOOL_LINK_STATIC_RUNTIME=ON

cmake --build build-centos7 --target release_proxy_tool release_proxy_server -j 4
```

## Run

Start SOCKS5 helper:

```bash
./build/release/proxy/release_proxy_tool serve 1080
```

Start HTTP proxy helper:

```bash
./build/release/proxy/release_proxy_tool http-proxy 3128
```

Start release proxy service:

```bash
./build/release/proxy/release_proxy_server
./build/release/proxy/release_proxy_server release/proxy/config.json
```

## Verify

TCP through SOCKS5:

```bash
curl --socks5-hostname 127.0.0.1:1080 -I https://example.com
```

UDP relay locally:

```bash
./build/release/proxy/release_proxy_tool udp-echo 19090
./build/release/proxy/release_proxy_tool udp-probe 127.0.0.1 1080 127.0.0.1 19090 PING-UDP
```

If everything is working, probe prints `UDP_OK`.

## Soak and readiness docs

- `release/proxy/PRODUCTION_READINESS.md`
- `release/proxy/SOAK_GUIDE.md`
- `release/proxy/run_proxy_soak.ps1`
- `release/proxy/run_proxy_stream_soak.ps1`
- `release/proxy/analyze_proxy_soak.ps1`
