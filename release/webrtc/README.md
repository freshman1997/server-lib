# release/webrtc

This directory contains release-oriented runnable assets for a lightweight WebRTC signaling/runtime server.

## Binary

- `release_webrtc_server`: TCP JSON-line server backed by `WebrtcPeerSession`

Default build output:

- `build/release/webrtc/release_webrtc_server`

## Protocol (JSON lines)

Each line is one JSON object.

Signaling apply (forward to `apply_signaling_json`):

```json
{"type":"offer","sdp":"...","as_remote":true}
{"type":"answer","sdp":"...","as_remote":false}
{"type":"candidate","candidate":"candidate:...","mid":"0","mline_index":0,"as_remote":true}
```

Control commands:

```json
{"cmd":"health"}
{"cmd":"snapshot"}
{"cmd":"runtime_config_get"}
{"cmd":"runtime_config_set","config":{"diagnostics_release_mode_strict_v2":true}}
```

## Server Config

Default config file:

- `release/webrtc/config.json`

Key fields:

- `host`
- `port`
- `probe_host`
- `local_ssrc`
- `clock_rate`
- `diagnostics_keep_latest_only`
- `nomination_max_pending_signals`
- `diagnostics_max_pending_signals`
- `diagnostics_emit_flat_compat_fields`
- `diagnostics_release_mode_strict_v2`
- `diagnostics_rollout_alert_window_seconds`
- `diagnostics_rollout_mismatch_count_alert_threshold`
- `diagnostics_rollout_mismatch_ratio_threshold_ppm`
- `health_log_interval_ms`

Environment overrides:

- `YUAN_WEBRTC_CONFIG`
- `YUAN_WEBRTC_PORT`

Server options:

```bash
build/release/webrtc/release_webrtc_server --config release/webrtc/config.json --port 9000
build/release/webrtc/release_webrtc_server --host 0.0.0.0 --port 9000
build/release/webrtc/release_webrtc_server --self-check-only --probe-host 127.0.0.1 --port 9000
```

## Quick Run

Start server:

```bash
build/release/webrtc/release_webrtc_server --config release/webrtc/config.json
```

Or use helper scripts after building:

```bash
bash release/webrtc/start.sh
bash release/webrtc/stop.sh
```

Windows PowerShell helpers:

```powershell
pwsh -File release/webrtc/start.ps1 -BuildDir build-mingw
pwsh -File release/webrtc/stop.ps1
```

## Health Check

Script:

- `release/webrtc/health_check.sh`
- `release/webrtc/health_check.ps1`

Run:

```bash
bash release/webrtc/health_check.sh
```

```powershell
pwsh -File release/webrtc/health_check.ps1 -BuildDir build-mingw -WebrtcPort 9000
```

## Smoke Test

`ctest` target:

- `release_webrtc_server_smoke`

Enable and run:

```bash
YUAN_RUN_RELEASE_WEBRTC_SMOKE=1 ctest --test-dir build -R release_webrtc_server_smoke --output-on-failure
```

```powershell
$env:YUAN_RUN_RELEASE_WEBRTC_SMOKE = "1"
ctest --test-dir build-mingw -R release_webrtc_server_smoke --output-on-failure
```
