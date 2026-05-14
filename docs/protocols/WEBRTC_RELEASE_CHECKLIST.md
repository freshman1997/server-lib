# WebRTC Release Checklist

## 1. Scope Freeze

- Release owner: TBD
- Target commit: `11ed7c1f` (working baseline)
- Release date: 2026-05-14
- Scope notes:
  - Diagnostics contract frozen for RC: nested-only default, strict-v2 enforcement, rollout threshold/decision fields, snapshot parity.
  - Service-facing runtime config JSON + rollout health JSON APIs added for operations integration.

### 1.1 Diagnostics Contract Freeze (RC)

- Producer default: nested-only diagnostics (`emit_flat_compat_fields=false`).
- Strict mode contract: `release_mode_strict_v2=true` enforces v2 + nested-only and cannot be downgraded by payload flags.
- Rollout threshold fields are frozen:
  - `rollout_alert_window_seconds`
  - `rollout_mismatch_count_alert_threshold`
  - `rollout_mismatch_ratio_threshold_ppm`
- Rollout decision fields are frozen:
  - `rollout_current_mismatch_ratio_ppm`
  - `rollout_alert_active`
  - `rollout_progress_blocked`
  - `rollout_ready_for_progress`
- Snapshot JSON must expose the same rollout threshold/decision fields with deterministic roundtrip behavior.

## 2. Required Commands

### 2.1 Build

```powershell
cmake --build "build-mingw" --target test_webrtc_bridge test_webrtc_sdp --config Debug
```

### 2.2 Blocking Gate

```powershell
ctest --test-dir "build-mingw" -R "^(webrtc_bridge|webrtc_sdp)$" --output-on-failure
ctest --test-dir "build-mingw" -R "^(rtp_session|rtp_session_manager|rtcp_session|rtcp_loopback|webrtc_bridge|webrtc_sdp)$" --output-on-failure
```

## 3. Pass Criteria (Release Blocking)

- Gate pass rate: 100%.
- No regression in strict-v2 and nested-only diagnostics behavior.
- Rollout threshold/decision fields parse/serialize and snapshot roundtrip are green.
- No deterministic error-contract drift in signaling parse/apply paths.

## 4. Artifacts to Attach

- Build log for `test_webrtc_bridge` and `test_webrtc_sdp`.
- CTest output for focused and combined regex gates.
- Final RC scope notes (target commit + contract freeze confirmation).

### 4.1 Current Gate Result (2026-05-14)

- Build: PASS
  - `cmake --build "build-mingw" --target test_webrtc_bridge test_webrtc_sdp --config Debug`
- Focused gate: PASS (2/2)
  - `ctest --test-dir "build-mingw" -R "^(webrtc_bridge|webrtc_sdp)$" --output-on-failure`
- Combined gate subset: PASS (6/6)
  - `ctest --test-dir "build-mingw" -R "^(rtp_session|rtp_session_manager|rtcp_session|rtcp_loopback|webrtc_bridge|webrtc_sdp)$" --output-on-failure`

## 5. Risk Sign-off

- Protocol owner: PENDING
- Test owner: PENDING
- Ops owner: PENDING
- Final decision: HOLD (awaiting sign-off)
