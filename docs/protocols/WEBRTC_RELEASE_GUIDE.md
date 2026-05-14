# WebRTC Release Guide

## Goal

Ship a deterministic WebRTC diagnostics contract that is nested-only by default, strict-v2 capable, and rollout-gated by machine-readable threshold and readiness signals.

## Contract Summary

- Default emission is nested-only:
  - `emit_flat_compat_fields=false`
- Strict release mode:
  - `release_mode_strict_v2=true` requires `schema_version="v2"` and nested-only payloads
  - strict mode cannot be downgraded by incoming payload flags
- Rollout threshold fields:
  - `rollout_alert_window_seconds`
  - `rollout_mismatch_count_alert_threshold`
  - `rollout_mismatch_ratio_threshold_ppm`
- Rollout decision fields:
  - `rollout_current_mismatch_ratio_ppm`
  - `rollout_alert_active`
  - `rollout_progress_blocked`
  - `rollout_ready_for_progress`

## Build and Verify (MinGW)

```powershell
cmake --build "build-mingw" --target test_webrtc_bridge test_webrtc_sdp --config Debug
ctest --test-dir "build-mingw" -R "^(webrtc_bridge|webrtc_sdp)$" --output-on-failure
ctest --test-dir "build-mingw" -R "^(rtp_session|rtp_session_manager|rtcp_session|rtcp_loopback|webrtc_bridge|webrtc_sdp)$" --output-on-failure
```

## Migration Timeline (Flat Field Deprecation)

- Phase A (observe): nested-only default, non-strict parser where needed, monitor rollout counters/signals.
- Phase B (stabilize): keep `mismatch_count == 0` for 7 consecutive days in each target environment.
- Phase C (enforce): enable `release_mode_strict_v2=true` for production traffic.
- Phase D (cleanup): remove flat parser compatibility only after all downstream consumers certify nested-only readiness.

## Rollback Playbook

- If rollout alerts trigger:
  - set `diagnostics_release_mode_strict_v2=false` for affected scope
  - keep nested-only default emission unchanged
  - continue collecting migration telemetry and readiness signals
- Rollout may continue only when:
  - `rollout_ready_for_progress=true`
  - and blocking alerts are resolved in target environment

## Suggested Release Notes Template

- Changed:
  - WebRTC diagnostics now emit nested-only by default.
  - strict-v2 release mode is available and enforced when enabled.
  - rollout thresholds and readiness signals are available in diagnostics payload and snapshot JSON.
- Compatibility:
  - flat compatibility fields remain parser-compatible during migration window.
  - duplicate nested/flat values must match when both are present.
