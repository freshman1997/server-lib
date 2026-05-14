# WEBRTC Design (M5 Foundation)

## Scope

- Build a deterministic WebRTC foundation on top of existing `RtcProto` and `RtcpProto` modules.
- Separate responsibilities into signaling, transport, and orchestration layers.
- Keep the stack test-first and timeline-driven before introducing real network engines.

## Architecture

### 1) Signaling Layer

`WebrtcSignalingBridge` handles signaling state, SDP validation, and signaling JSON mapping.

- State machine:
  - `new_`
  - `have_local_offer`
  - `have_remote_offer`
  - `stable`
- Message kinds:
  - `offer`, `answer`, `candidate`, `rollback`
  - transport-state hints: `ice_state`, `dtls_state`
  - ICE nomination signal: `ice_nomination`
  - security-state signal: `security_state`
- Strict SDP commit path for local/remote description:
  1. reject empty SDP
  2. reject invalid signaling transition
  3. parse SDP using `WebrtcSdp::parse`
  4. commit description + parsed cache + state only on success
- Fixed error codes for deterministic behavior:
  - `empty_sdp`
  - `invalid_state_transition`
  - `sdp_parse_failed`
  - `sdp_media_count_mismatch`
  - `sdp_media_kind_mismatch`
  - `sdp_mid_mismatch`
  - `sdp_payload_mismatch`
  - success clears error to `""`

Negotiation validation currently enforces deterministic answer-vs-offer compatibility:

- media section count must match
- media `kind` must match by index
- `mid` must match by index
- each media section must keep at least one common payload type

Security signaling uses enum-first semantics while preserving legacy compatibility:

- `security_code` (canonical) is parsed first when present
- `security_error` remains for backward compatibility and custom external reasons
- canonical codes map to:
  - `none`
  - `dtls_fingerprint_mismatch`
  - `external_security_error`

Exposed observability APIs:

- `has_parsed_local_sdp()` / `has_parsed_remote_sdp()`
- `parsed_local_sdp()` / `parsed_remote_sdp()`
- `last_sdp_error()`

### 2) SDP Layer

`WebrtcSdp` provides a minimal parser/serializer used by signaling strict mode.

- Supported session/media elements:
  - `v/o/s/t`
  - `a=group:BUNDLE`
  - `m=` sections
  - `a=mid`
  - direction attributes (`sendrecv`, etc.)
  - `a=rtpmap`
- Goal is deterministic validation and roundtrip safety, not full RFC parity yet.

### 3) Transport Layer

`WebrtcTransportBridge` binds RTP/RTCP session logic to WebRTC runtime behavior.

- Owns:
  - `RtpSessionManager` (inbound RTP tracking)
  - `RtcpSession` (RR/SR generation)
- Deterministic RTCP scheduler:
  - `RtcpScheduleConfig`
  - tick-based polling via `poll_scheduled_rtcp(now_ms, out)`
  - sender report cadence via `sender_report_every`
  - RR snapshot reset passthrough via `rr_snapshot_reset_interval_reports`

### 4) ICE/DTLS Mock Transport Abstractions

To unblock M5 flow without external dependencies, mock engines are used.

- `MockIceTransport`
  - states: `new_ -> checking -> connected|failed`
  - candidate-triggered checking (configurable)
  - deterministic connect/fail timing via config
  - adapter seam for next phase real ICE integration:
    - `IceTransportAdapter` lifecycle (`start_gathering`, `start_checklist`, `poll`)
    - explicit gathering/checklist states:
      - gathering: `new_`, `gathering`, `complete`
      - checklist: `idle`, `running`, `completed`, `failed`
  - scaffold adapter (`ScaffoldIceAdapter`) currently simulates host-candidate gather and checklist completion
  - pluggable provider seam (`IceTransportProvider`) for future real STUN/TURN/checklist integration
  - nomination observability:
    - nomination state: `none`, `in_progress`, `nominated`, `failed`
    - selected pair reason: `highest_priority`, `nominated_by_provider`, `forced_by_signal`
    - transport-level last ICE error string for deterministic diagnostics
- `MockDtlsTransport`
  - states: `new_ -> connecting -> connected|failed`
  - can auto-start when ICE becomes connected
  - deterministic handshake timing via config

### 5) SRTP Hook Layer (Placeholder)

`SrtpContext` defines protected/unprotected packet seams for future real SRTP integration.

- Interfaces:
  - `protect_rtp`, `unprotect_rtp`
  - `protect_rtcp`, `unprotect_rtcp`
- `MockSrtpContext` currently provides:
  - configurable pass/fail behavior per direction/type
  - call counters for assertion in tests

Transport integration points:

- inbound RTP path can run through `unprotect_rtp` before RTP stats/session ingestion
- scheduled RTCP path runs through `protect_rtcp` before emit

### 6) Peer Orchestration Layer

`WebrtcPeerSession` composes signaling + transport + mock ICE/DTLS + runtime state.

- Tracks:
  - signaling state
  - ICE transport state
  - DTLS transport state
  - peer connection state (`new_`, `connecting`, `connected`, `failed`)
- Readiness gates:
  - transport ready = ICE connected + DTLS connected
  - media ready = signaling stable + transport ready
- Scheduler activation:
  - RTCP scheduler activates only when media ready
  - schedule reset on transition inactive -> active
- Supports both:
  - automatic mock transport progression (`advance_transport(now_ms)`)
  - explicit state override via signaling `ice_state`/`dtls_state`
  - Snapshot observability:
    - `snapshot()`
    - `snapshot_json()` / `parse_snapshot_json()`
    - connection state callback for lifecycle transitions
    - ICE-specific fields:
      - gathering/checklist/nomination states
      - selected-pair presence + reason metadata
      - last ICE error
      - STUN transaction observability:
        - transaction history (`stun_transactions`)
        - last transaction summary (`has_last_stun_transaction`, `last_stun_transaction`)

### 7) STUN Transaction Abstraction (Provider/Adapter/Engine)

The ICE provider seam now includes a deterministic STUN transaction model for request/response/timeout
observability across provider-backed and scaffold paths.

- Transaction model:
  - `StunTransaction`
    - `transaction_id`
    - `state`: `new_`, `request_sent`, `response_received`, `timed_out`, `failed`
    - request timeline: `started_at_ms`, `last_request_at_ms`, `completed_at_ms`
    - retry counters: `request_count`, `retransmit_count`
    - request metadata: `request_priority`, `request_use_candidate`
    - `error`
    - `response_code`
    - `mapped_address`, `mapped_port`
- Provider contract (`IceTransportProvider`) now exposes:
  - `stun_transactions()`
  - `has_last_stun_transaction()`
  - `last_stun_transaction()`
- Adapter and engine propagate these fields unchanged so peer snapshot JSON remains deterministic.

Mock behavior:

- `MockIceProvider` now models a STUN request at checklist start.
- Configurable deterministic knobs:
  - `stun_response_delay_ms`
  - `stun_timeout_ms`
  - `stun_retry_interval_ms`
  - `stun_max_retransmits`
  - `force_stun_timeout`
- Supports multiple active transactions (for repeated candidate/checklist events).
- Success path produces `response_received` with mapped endpoint and `response_code=success`.
- Timeout path produces `timed_out`, sets error (`stun_timeout`), `response_code=timeout`, and fails checklist with
  `provider_stun_timeout`.

### 8) Nomination Signaling Override

`ice_nomination` signaling enables control-plane nomination updates without forcing SDP or ICE restart.

- JSON fields:
  - `type = "ice_nomination"`
  - `state` (`none` / `in_progress` / `nominated` / `failed`)
  - optional `selected_pair_reason`
  - optional `selected_pair_reason_text`
  - optional `nomination_transaction_id` (links control-plane nomination to observed STUN transaction)
- `WebrtcPeerSession` forwards this to ICE engine via
  `set_nomination_from_signal(state, reason, reason_text, nomination_transaction_id)`.
- `WebrtcPeerSession` can emit outbound nomination events through
  `poll_ice_nomination_signal(...)` when nomination state/linked transaction changes.
  - semantics: queued FIFO events; each `poll` returns at most one pending message
  - duplicate suppression remains state+transaction-id based before enqueue
  - bounded queue (`kMaxPendingIceNominationSignals=16`) with oldest-drop strategy on overflow
  - overflow observability via snapshot counters:
    - `pending_ice_nomination_signal_count`
    - `peak_pending_ice_nomination_signal_count`
    - `dropped_ice_nomination_signal_count`
    - `dropped_ice_nomination_signal_overflow_count`
    - `dropped_ice_nomination_signal_trim_count`

Queue capacity is now configurable at runtime via `NominationSignalQueueConfig` on
`WebrtcPeerSession` (`set_nomination_signal_queue_config` / `nomination_signal_queue_config`).
Invalid `max_pending_signals=0` is clamped to `1`.
Drop counters are split by cause:

- `dropped_ice_nomination_signal_overflow_count`: enqueue-time drops due to full queue
- `dropped_ice_nomination_signal_trim_count`: drops caused by runtime queue-size reconfiguration
- `dropped_ice_nomination_signal_count`: total (`overflow + trim`)
- `MockIceTransport` applies nomination override deterministically:
  - marks nomination state
  - creates/updates selected pair reason metadata
  - keeps reason text as provided when present
  - stores linked transaction id on selected pair metadata
  - reports deterministic error `nomination_transaction_not_found` when provided id is not found in observed STUN history

### 9) Diagnostics Signal

`WebrtcSignalingBridge` now supports a lightweight `diagnostics` message kind for control-plane pull/observe workflows.

- type: `diagnostics`
- current producer: `WebrtcPeerSession::poll_diagnostics_signal(...)`
- diagnostics queue mode is configurable at runtime via `DiagnosticsSignalQueueConfig`:
  - `keep_latest_only=true` (default): keep only latest diagnostics snapshot
  - `keep_latest_only=false`: bounded FIFO queue with `max_pending_signals`
- unified runtime queue config is available via `SignalQueueRuntimeConfig` on `WebrtcPeerSession`:
  - `nomination_max_pending_signals`
  - `diagnostics_keep_latest_only`
  - `diagnostics_max_pending_signals`
  - `diagnostics_emit_flat_compat_fields`
  - `diagnostics_release_mode_strict_v2`
  - `diagnostics_rollout_alert_window_seconds`
  - `diagnostics_rollout_mismatch_count_alert_threshold`
  - `diagnostics_rollout_mismatch_ratio_threshold_ppm`
  - internally delegates to existing nomination/diagnostics queue setters (including clamp/trim semantics)
  - also applies diagnostics emission behavior to `WebrtcSignalingBridge::DiagnosticsEmissionConfig`
  - runtime config JSON helpers are available:
    - `signal_queue_runtime_config_json()`
    - `parse_signal_queue_runtime_config_json(...)`
    - parser clamps invalid low values to safe minimums (`>=1` for queue/window limits)
- payload fields include:
  - `schema_version`:
    - emitted as `"v2"` by current producer
    - parser accepts `"v1"` and `"v2"`
    - when absent, parser infers `v2` if nested objects are present, otherwise infers `v1`
    - when explicitly `"v2"`, nested objects are required (`nomination`, `stun`, `queues`, `policy`)
      and flat fields are treated as optional compatibility duplicates
    - when explicitly `"v1"`, flat fields are required (nested objects may be present as redundant data)
    - when both nested and flat duplicates are present for the same metrics, values must match
  - `emit_flat_compat_fields`:
    - reflects producer emission mode in diagnostics payload
    - when `false`, producer forces `schema_version` to `"v2"`
    - default producer/runtime value is `false` (nested-only by default)
    - parser rejects incompatible combination `emit_flat_compat_fields=false` with `schema_version="v1"`
  - `release_mode_strict_v2`:
    - reflects producer release mode in diagnostics payload
    - when `true`, producer emits nested-only diagnostics and forces `schema_version` to `"v2"`
    - runtime parser strict mode cannot be downgraded by payload flags
    - parser rejects incompatible combinations where `release_mode_strict_v2=true` but schema/emission are not strict v2
  - `scope`
  - nomination summary (`ice_nomination_state`, `has_selected_pair`, `selected_pair_reason`, `selected_pair_reason_text`)
  - `nomination` nested object is now emitted with equivalent fields:
    - `state`, `has_selected_pair`, `selected_pair_reason`, `selected_pair_reason_text`
  - ICE/STUN summary (`last_ice_error`, `stun_transaction_count`, `has_last_stun_transaction`, `last_stun_transaction_id`, `last_stun_transaction_state`)
  - `stun` nested object is now emitted with equivalent fields:
    - `last_ice_error`, `transaction_count`, `has_last_transaction`, `last_transaction_id`, `last_transaction_state`
  - queue summary (`pending_nomination_signal_count`, `dropped_nomination_signal_count`,
    `dropped_nomination_signal_overflow_count`, `dropped_nomination_signal_trim_count`)
  - diagnostics-queue summary (`pending_diagnostics_signal_count`, `dropped_diagnostics_signal_count`,
    `dropped_diagnostics_signal_overflow_count`, `dropped_diagnostics_signal_trim_count`)
  - `queues` nested object is now emitted with equivalent queue/drop fields:
    - `nomination.pending_signal_count`, `nomination.dropped_signal_count`,
      `nomination.dropped_signal_overflow_count`, `nomination.dropped_signal_trim_count`
    - `diagnostics.pending_signal_count`, `diagnostics.dropped_signal_count`,
      `diagnostics.dropped_signal_overflow_count`, `diagnostics.dropped_signal_trim_count`
  - queue policy summary (`policy_keep_latest_only`, `policy_max_pending_signals`,
    `policy_nomination_max_pending_signals`)
  - `policy` nested object is now emitted with equivalent fields:
    - `keep_latest_only`
    - `max_pending_signals`
    - `nomination_max_pending_signals`
  - parser accepts both formats for compatibility:
    - legacy flat `policy_*` fields
    - nested `policy` object
  - parser also accepts both queue formats for compatibility:
    - legacy flat queue fields (`pending_*`, `dropped_*`)
    - nested `queues` object
  - parser also accepts both nomination/STUN formats for compatibility:
    - legacy flat nomination and STUN fields
    - nested `nomination` and `stun` objects
  - bridge telemetry counters for migration observability:
    - `diagnostics_v2_flat_duplicate_seen_count()`
    - `diagnostics_v2_flat_duplicate_mismatch_count()`
  - diagnostics payload also emits `migration` nested object:
    - `migration.v2_flat_duplicate_seen_count`
    - `migration.v2_flat_duplicate_mismatch_count`
  - diagnostics emission can be configured via `DiagnosticsEmissionConfig`:
    - `emit_flat_compat_fields=false` (default): emit nested fields only (flat compatibility fields omitted)
    - `emit_flat_compat_fields=true`: emit both flat and nested fields
  - parser accepts both migration formats:
    - flat fields (`v2_flat_duplicate_seen_count`, `v2_flat_duplicate_mismatch_count`)
    - nested `migration` object
- snapshot additionally exposes diagnostics queue counters:
  - `pending_diagnostics_signal_count`, `peak_pending_diagnostics_signal_count`
  - `dropped_diagnostics_signal_count`, `dropped_diagnostics_signal_overflow_count`, `dropped_diagnostics_signal_trim_count`
  - diagnostics duplicate-migration telemetry counters:
    - `diagnostics_v2_flat_duplicate_seen_count`
    - `diagnostics_v2_flat_duplicate_mismatch_count`
  - diagnostics queue policy snapshot fields:
    - `diagnostics_policy_keep_latest_only`
    - `diagnostics_policy_max_pending_signals`
    - `diagnostics_policy_nomination_max_pending_signals`
  - diagnostics emission mode snapshot fields:
    - `diagnostics_emit_flat_compat_fields`
    - `diagnostics_release_mode_strict_v2`
  - diagnostics rollout threshold snapshot fields:
    - `diagnostics_rollout_alert_window_seconds`
    - `diagnostics_rollout_mismatch_count_alert_threshold`
    - `diagnostics_rollout_mismatch_ratio_threshold_ppm`
  - diagnostics rollout decision snapshot fields:
    - `diagnostics_rollout_current_mismatch_ratio_ppm`
    - `diagnostics_rollout_alert_active`
    - `diagnostics_rollout_progress_blocked`
    - `diagnostics_rollout_ready_for_progress`
  - rollout health surface:
    - `rollout_health_json()` returns operator-facing readiness payload
    - includes `ready` and `rollout_ready_for_progress` aliases plus threshold/decision counters

### Flat-Field Rollout Plan

- Goal:
  - transition to nested-only diagnostics as the long-term contract
  - retain controlled compatibility window for consumers that still require flat fields
- Runtime controls:
  - `diagnostics_emit_flat_compat_fields` (default `false`) toggles flat compatibility emission
  - `diagnostics_release_mode_strict_v2=true` enforces strict nested-only/v2 semantics at parser+emitter boundaries
- Telemetry sources (per bridge/session snapshot):
  - `diagnostics_v2_flat_duplicate_seen_count`
  - `diagnostics_v2_flat_duplicate_mismatch_count`
  - payload-level `migration.v2_flat_duplicate_seen_count`
  - payload-level `migration.v2_flat_duplicate_mismatch_count`
- Suggested migration window gates:
  - Phase A (observe): keep non-strict parser mode where needed, emit nested-only by default, monitor counters
  - Phase B (stabilize): require `mismatch_count == 0` for 7 consecutive days in each target environment
  - Phase C (enforce): enable `release_mode_strict_v2=true` for production traffic
  - Phase D (cleanup): remove flat-field parser acceptance only after all integrated consumers certify nested-only
- Alert thresholds (recommended):
  - `v2_flat_duplicate_mismatch_count > 0` in any 24h window: page owner/on-call
  - mismatch ratio `mismatch_count / max(1, seen_count) > 0.001` (0.1%) in any 24h window: block rollout progression
  - `seen_count` non-decreasing while strict mode is enabled should be treated as compatibility traffic debt and tracked
- Rollout threshold fields (machine-readable):
  - flat compatibility fields:
    - `rollout_alert_window_seconds`
    - `rollout_mismatch_count_alert_threshold`
    - `rollout_mismatch_ratio_threshold_ppm`
    - `rollout_current_mismatch_ratio_ppm`
    - `rollout_alert_active`
    - `rollout_progress_blocked`
    - `rollout_ready_for_progress`
  - nested object `rollout_policy` fields:
    - `alert_window_seconds`
    - `mismatch_count_alert_threshold`
    - `mismatch_ratio_threshold_ppm`
    - `current_mismatch_ratio_ppm`
    - `alert_active`
    - `progress_blocked`
    - `ready_for_progress`
  - parser accepts both flat and nested formats; if both are present for the same field, values must match
  - defaults:
    - `rollout_alert_window_seconds = 86400`
    - `rollout_mismatch_count_alert_threshold = 0`
    - `rollout_mismatch_ratio_threshold_ppm = 1000`
    - `rollout_current_mismatch_ratio_ppm = 0`
    - `rollout_alert_active = false`
    - `rollout_progress_blocked = false`
    - `rollout_ready_for_progress = true`
  - decision semantics:
    - `current_mismatch_ratio_ppm = mismatch_count * 1_000_000 / max(1, seen_count)`
    - `alert_active = mismatch_count > mismatch_count_alert_threshold`
    - `rollout_progress_blocked = alert_active || current_mismatch_ratio_ppm > mismatch_ratio_threshold_ppm`
    - `rollout_ready_for_progress = !rollout_progress_blocked`
- Rollback guidance:
  - if alerts trigger during Phase B/C, disable strict mode (`diagnostics_release_mode_strict_v2=false`) for affected scope
  - keep nested emission default unchanged; only relax parser strictness while remediating producers/consumers

## Test Strategy and Status

### Unit/Integration Coverage

- Signaling strict SDP validation and state transitions.
- Offer/answer negotiation hardening:
  - media count / kind / MID compatibility
  - payload-type compatibility including PT->`rtpmap` mapping consistency
- SDP parse/serialize roundtrip.
- ICE candidate parsing and adapter-lifecycle observability (gather/checklist state progression).
- RTP->RTCP bridge correctness (RR/SR content and schedule behavior).
- Peer lifecycle timeline (offer/answer, candidate, ICE/DTLS transitions).
- Signaling negotiation mismatch errors (media count/kind/MID/payload).
- Security-state JSON precedence (`security_code` first) and legacy fallback paths.
- STUN transaction observability in snapshot JSON.
- ICE nomination signaling parse/serialize and peer apply path (`ice_nomination`).
- Nomination/STUN linking via `nomination_transaction_id` and selected-pair metadata propagation.
- Outbound nomination event polling with duplicate suppression semantics.
- Outbound nomination event queue (FIFO) to avoid dropping rapid transitions.
- Diagnostics signaling JSON parse/serialize and peer diagnostics polling path.
- Mock ICE/DTLS deterministic auto-progression.
- SRTP hook success/failure paths on RTP receive and RTCP emit.
- Runtime snapshot JSON roundtrip and state consistency assertions.

### Current Regression Baseline

Focused MinGW regression suite passes:

- `rtp_session`
- `rtp_session_manager`
- `rtcp_session`
- `rtcp_loopback`
- `webrtc_bridge`
- `webrtc_sdp`

## Release Candidate Scope

- Contract freeze targets (diagnostics path):
  - nested objects are the default producer contract (`emit_flat_compat_fields=false`)
  - `schema_version` explicit v1/v2 behavior is strict and test-covered
  - `release_mode_strict_v2` enforces nested-only + v2 semantics and cannot be downgraded by payload flags
  - rollout threshold fields and rollout decision fields are part of the frozen diagnostics contract
  - snapshot JSON contains the same rollout threshold/decision signals for deterministic observability
- Compatibility window policy:
  - parser continues accepting flat compatibility fields during migration window
  - when flat+nested duplicates are both present, values must match or parse fails
  - migration telemetry counters are retained for rollout monitoring and deprecation gating
- Out of RC scope:
  - replacing mock ICE/DTLS engines with production RFC-complete stacks
  - browser interop matrix and NAT/TURN large-scale validation
  - SRTP cryptographic implementation beyond existing hook seams

## Known Limits

- ICE and DTLS are mock engines, not RFC-complete stacks.
- SRTP currently defines hook seams; it does not perform cryptographic protection yet.
- SDP parser intentionally supports a minimal subset required by current negotiation flow.

## Next Milestones

- Replace mock ICE with real candidate gathering/checklist/pair selection engine.
- Replace mock DTLS with real handshake + key export path.
- Bind real SRTP crypto contexts to existing hook seams.
- Extend SDP negotiation constraints (media/mid/payload compatibility hardening).
- Add browser interop and NAT/TURN validation scenarios.
