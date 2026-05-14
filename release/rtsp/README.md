# release/rtsp

This directory contains release-oriented runnable assets for RTSP.

## Binary

- `release_rtsp_server`: RTSP server entry with config, env, and CLI options

Default build output:

- `build/release/rtsp/release_rtsp_server`

## Server Config

Default config file:

- `release/rtsp/config.json`

Key fields:

- `port`
- `app_name`
- `enable_log`
- `enable_audit`
- `max_audit_events`
- `udp_retry_max_retries`
- `udp_retry_base_backoff_ms`
- `udp_retry_max_backoff_ms`

Environment overrides:

- `YUAN_RTSP_CONFIG`
- `YUAN_RTSP_PORT`
- `YUAN_RTSP_APP_NAME`
- `YUAN_RTSP_ENABLE_LOG`
- `YUAN_RTSP_ENABLE_AUDIT`
- `YUAN_RTSP_MAX_AUDIT_EVENTS`
- `YUAN_RTSP_UDP_RETRY_MAX`
- `YUAN_RTSP_UDP_RETRY_BASE_MS`
- `YUAN_RTSP_UDP_RETRY_MAX_MS`

Server options:

```bash
build/release/rtsp/release_rtsp_server --config release/rtsp/config.json --port 8554
build/release/rtsp/release_rtsp_server -f release/rtsp/config.json --enable-log true --enable-audit true
```

## Quick Run

Start server:

```bash
build/release/rtsp/release_rtsp_server --config release/rtsp/config.json
```

Or use helper scripts after building:

```bash
bash release/rtsp/start.sh
bash release/rtsp/stop.sh
```

Windows PowerShell helpers:

```powershell
pwsh -File release/rtsp/start.ps1 -BuildDir build-mingw
pwsh -File release/rtsp/stop.ps1
```

## Validation Scripts

Scripts index:

- `release/rtsp/scripts/README.md`

Examples:

```bash
bash release/rtsp/scripts/rtsp_preflight.sh
bash release/rtsp/scripts/run_rtsp_gate.sh build "rtsp|rtcp"
bash release/rtsp/scripts/run_rtsp_soak.sh build 3600 4 1 rtsp ./logs/rtsp_soak
```

PowerShell examples:

```powershell
pwsh -File release/rtsp/scripts/rtsp_preflight.ps1
pwsh -File release/rtsp/scripts/run_rtsp_gate.ps1 -BuildDir build-mingw -Regex "rtsp|rtcp"
```

## Smoke Test

`ctest` target:

- `release_rtsp_server_smoke`

Enable and run:

```bash
YUAN_RUN_RELEASE_RTSP_SMOKE=1 ctest --test-dir build -R release_rtsp_server_smoke --output-on-failure
```

```powershell
$env:YUAN_RUN_RELEASE_RTSP_SMOKE = "1"
ctest --test-dir build-mingw -R release_rtsp_server_smoke --output-on-failure
```

## Health Check

Script:

- `release/rtsp/health_check.sh`
- `release/rtsp/health_check.ps1`

Run:

```bash
bash release/rtsp/health_check.sh
```

```powershell
pwsh -File release/rtsp/health_check.ps1 -BuildDir build-mingw -RtspPort 554
```

Optional env:

- `BUILD_DIR`
- `SERVER_BIN`
- `RTSP_PORT`

## Build For Other Linux Machines

Default build links `libstdc++/libgcc` statically on Linux for better portability.

For maximum portability, build with musl toolchain:

```bash
bash build_release_musl.sh
```
