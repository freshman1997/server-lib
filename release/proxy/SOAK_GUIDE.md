# Proxy Soak Guide

This guide runs sustained load and checks memory/traffic convergence.

## Prerequisites

- Build proxy server binary:

```powershell
cmake --build build --target release_proxy_server -j 4
```

- Make sure `curl.exe` is available in PATH.

## A) General HTTP soak

```powershell
.\release\proxy\run_proxy_soak.ps1 `
  -ProxyServerPath .\build\release\proxy\release_proxy_server.exe `
  -ProxyPort 3128 `
  -DurationSec 1800 `
  -Concurrency 8
```

Analyze:

```powershell
.\release\proxy\analyze_proxy_soak.ps1 -ProxyLog .\logs\proxy_soak\proxy-<timestamp>.log -IdleTailSeconds 30
```

## B) Streaming/video-like soak

This starts a local chunked streaming upstream and repeatedly pulls it through proxy.

```powershell
.\release\proxy\run_proxy_stream_soak.ps1 `
  -ProxyServerPath .\build\release\proxy\release_proxy_server.exe `
  -ProxyPort 3128 `
  -UpstreamPort 18080 `
  -DurationSec 1800 `
  -Concurrency 6 `
  -ClientStreamSec 30
```

Analyze with the same analyzer:

```powershell
.\release\proxy\analyze_proxy_soak.ps1 -ProxyLog .\logs\proxy_stream_soak\proxy-<timestamp>.log -IdleTailSeconds 30
```

## Read the result

- `idle convergence : PASS` means tail has `active=0` and zero throughput.
- `memory trend : PASS` means tail process memory is stable or decreasing.
- `WARN` means investigate logs around last active sessions and close reasons.
