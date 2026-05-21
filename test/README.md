# Test Tree

This directory is organized by feature area:

- `core/` core runtime, buffers, logging, coroutine, and utility tests
- `network/` network-level helper tests
- `plugin/` plugin system tests
- `protocol/http/` HTTP tests
- `protocol/dns/` DNS tests
- `protocol/ftp/` FTP tests and helper scripts
- `protocol/bit_torrent/` BitTorrent tests
- `protocol/mqtt/` MQTT tests
- `protocol/websocket/` WebSocket tests
- `protocol/socks5/` SOCKS5 protocol tests
- `protocol/ssh/` SSH tests
- `protocol/smb/` SMB tests
- `proxy/` SOCKS5 proxy tool and usage guide

## Quick Start

- Build the proxy helper: `cmake --build . --target release_proxy_tool -j 4`
- Run the proxy: `.\build\release\proxy\release_proxy_tool.exe serve 1080`
- Verify TCP: `curl.exe --socks5-hostname 127.0.0.1:1080 -I https://example.com`
- Verify UDP locally:
  - `.\build\release\proxy\release_proxy_tool.exe udp-echo 19090`
  - `.\build\release\proxy\release_proxy_tool.exe udp-probe 127.0.0.1 1080 127.0.0.1 19090 PING-UDP`

## Build Test Groups Selectively

To reduce compile time, each major test area can be toggled independently:

- `YUAN_TEST_BUILD_CORE` (default `ON`)
- `YUAN_TEST_BUILD_NAS` (default `ON`)
- `YUAN_TEST_BUILD_NETWORK` (default `ON`)
- `YUAN_TEST_BUILD_PLUGIN` (default `ON`)
- `YUAN_TEST_BUILD_PROTOCOL` (default `ON`)
- `YUAN_TEST_BUILD_PROXY` (default `ON`)
- `YUAN_TEST_BUILD_BENCHMARK` (default `ON`)

Protocol tests also support per-module switches:

- `YUAN_TEST_BUILD_PROTOCOL_HTTP`, `YUAN_TEST_BUILD_PROTOCOL_DNS`, `YUAN_TEST_BUILD_PROTOCOL_BIT_TORRENT`
- `YUAN_TEST_BUILD_PROTOCOL_MQTT`, `YUAN_TEST_BUILD_PROTOCOL_WEBSOCKET`, `YUAN_TEST_BUILD_PROTOCOL_FTP`
- `YUAN_TEST_BUILD_PROTOCOL_SOCKS5`, `YUAN_TEST_BUILD_PROTOCOL_SSH`, `YUAN_TEST_BUILD_PROTOCOL_SMB`
- `YUAN_TEST_BUILD_PROTOCOL_SHADOWSOCKS`, `YUAN_TEST_BUILD_PROTOCOL_WEBDAV`

Example (only build core + network tests):

`cmake -S . -B build -D YUAN_BUILD_TESTS=ON -D YUAN_TEST_BUILD_CORE=ON -D YUAN_TEST_BUILD_NETWORK=ON -D YUAN_TEST_BUILD_PROTOCOL=OFF -D YUAN_TEST_BUILD_NAS=OFF -D YUAN_TEST_BUILD_PLUGIN=OFF -D YUAN_TEST_BUILD_PROXY=OFF -D YUAN_TEST_BUILD_BENCHMARK=OFF`

## Connection Ownership

The networking core now uses shared ownership for live connections:

- `net::ConnectionPtr` is the preferred handle type for newly created connections.
- `create_stream_connection(...)` and `create_datagram_connection(...)` return `ConnectionPtr`.
- `async_accept(...)` and `async_connect(...)` now hand back `shared_ptr`-owned connections.
- `AsyncConnectionContext` keeps the connection alive for the duration of the async session.

For new code, prefer `ConnectionPtr` over raw `Connection*` when you need to retain ownership past the current callback.

## FTP Scripts

FTP helper scripts live in:

- `protocol/ftp/scripts/run_ftp_e2e.bat`
- `protocol/ftp/scripts/test_ftp_e2e_quick.bat`
