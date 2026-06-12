# WebSocket Autobahn And Soak Verification

This document records the runnable verification commands for the WebSocket
production-readiness gate.

## Local CTest Coverage

The checked-in WebSocket tests cover protocol parsing, HTTP proxy handoff, and a
loopback e2e server with browser-like masked clients.

```bash
cmake --build build --target test_websocket_protocol test_http_websocket_proxy test_websocket_e2e
ctest -R "http_websocket_proxy|websocket_protocol|websocket_e2e" --output-on-failure
```

`websocket_e2e` includes:

- Browser-compatible RFC 6455 handshake with tokenized `Connection`, `Origin`,
  masked text/binary frames, ping/pong, and close reply.
- Slow incomplete handshake timeout coverage using a test-local
  `handshake_timeout` config.
- Concurrent connection/message throughput smoke coverage.
- Reconnect and mixed payload soak smoke coverage.

## Extended Soak Run

The e2e test supports a longer soak loop through `WEBSOCKET_SOAK_SECONDS`. For a
24-hour soak:

```bash
WEBSOCKET_SOAK_SECONDS=86400 ./build/bin/test_websocket_e2e
```

The default CTest run intentionally uses a short smoke soak so CI does not block
for hours. A production release should archive the 24-hour command output with
the release notes.

## Autobahn Testsuite

Autobahn is an external conformance suite. It is not vendored into this repo.
The repository includes a fuzzing-client config at
`docs/protocols/autobahn/fuzzingclient.json` and a dedicated local echo server
target, `test_websocket_autobahn_server`.

Build the echo server:

```bash
cmake --build build --target test_websocket_autobahn_server
```

Start it in one shell:

```bash
./build/bin/test_websocket_autobahn_server 12211
```

Run Autobahn from another shell with Docker:

```bash
sudo -n docker run --rm --network host \
  -v "$PWD/docs/protocols/autobahn:/reports" \
  crossbario/autobahn-testsuite \
  wstest -m fuzzingclient -s /reports/fuzzingclient.json
```

Save the generated report under `docs/protocols/autobahn/output/` and link it
from the release checklist.

Current environment note: Docker is installed and the daemon is reachable via
`sudo -n`, but the Autobahn image is not available locally and pulling
`crossbario/autobahn-testsuite` failed because the Docker registry connection was
refused. The Autobahn checklist item remains open until the suite runs and the
report is archived.
