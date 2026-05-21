# NAS Commercialization Plan

Last updated: 2026-05-11

This plan turns the current NAS implementation into a sellable private deployment product.
Execution principle: WebDAV first for production, SMB as a controlled beta track.

## 1) Commercial Target

- Product shape: private on-prem NAS gateway service.
- First sellable scope: WebDAV + admin API + audit + health + config reload.
- Explicitly out of first commercial SLA: full SMB client compatibility matrix, durable handles, multi-channel.

## 2) Release Gates

All items below must be green before external paid rollout.

### Gate A - Security Baseline (P0)

- Replace weak password formats with PBKDF2-HMAC-SHA256 (or stronger KDF).
- Keep backward-compatible login verification for migration window, then disable legacy formats by policy.
- Enforce request size/header limits and fail closed on malformed auth/metadata checks.
- Complete traversal/auth bypass regression tests.

Exit criteria:

- No `plain:` or `fnv1a64$...` hashes accepted in production mode.
- Security test suite passes for traversal/auth/header/body boundaries.

### Gate B - Protocol Correctness (P0)

- Finish WebDAV lock expiry lifecycle (prune expired, correct expiration mapping).
- Verify lock conflict behavior under concurrent write/move/delete.
- Keep share isolation strict and document unsupported cross-share semantics.

Exit criteria:

- Lock timeout behavior is deterministic in restart/concurrency tests.
- No stale-lock write denial after expiration window.

### Gate C - Operability (P0)

- Harden audit/session/admin APIs (retention policy, bounded queries, rate limiting).
- Finalize production runbook: startup, health checks, backup/restore, upgrade/rollback.
- Package deployment template (config, directories, bootstrap, default health probes).

Exit criteria:

- Fresh node can be deployed from runbook in under 30 minutes.
- Health endpoint and admin endpoint operational checks pass.

Current progress:

- Admin APIs now enforce bounded JSON body size.
- Audit/session endpoints support bounded `limit` query handling.
- Admin API now applies basic per-actor+remote rate limiting with 429 response.

### Gate D - Interop + Stability (P1)

- Build reproducible interop scripts: rclone, cadaver, Windows WebClient, macOS Finder.
- Run large-file soak and parallel WebDAV workload tests.
- Add crash-recovery checks for lock/session/temp file cleanup.

Exit criteria:

- Interop matrix results documented with pass/fail and known limitations.
- Soak and concurrency suites pass with no data loss.

### Gate E - SMB Commercial Beta (P2)

- Keep SMB optional and disabled by default for paid WebDAV rollout.
- Validate signing-required mode with Linux and Windows clients.
- Publish SMB compatibility statement per feature set.

Exit criteria:

- SMB beta checklist passes; SLA marked "best effort" until matrix is complete.

## 3) Workstreams And Task List

Priority order for implementation:

1. Replace weak NAS password hashing with PBKDF2-HMAC-SHA256 and migration path.
2. Implement WebDAV lock expiry pruning and correct expiry mapping.
3. Add ACL/path checks for all mutating WebDAV requests.
4. Close critical security gaps (traversal, auth bypass, header/body limits).
5. Harden audit/session/admin APIs with retention and rate limits.
6. Build interop matrix and scripts for WebDAV real clients.
7. Add stability tests: large-file soak, concurrency, crash-recovery.
8. Package private deployment bundle (config template, runbook, health checks, upgrade path).
9. SMB commercialization track: signing-required and Linux/Windows validation.

## 3.1) Readability, Modularity, Performance Rules

Apply these rules while implementing all workstreams:

- Readability:
  - Keep functions focused and side-effect scope clear.
  - Prefer descriptive names over compact logic.
  - Keep migration behavior explicit (`pbkdf2` default, legacy fallback separated).

- Modularity:
  - Extend `NasMetadataStore` interfaces for shared behavior (for example lock creation/pruning) instead of type-specific branching.
  - Keep WebDAV/SMB adapters thin and reusable around NAS core services.

- Performance:
  - Avoid repeated heavy operations in hot paths.
  - Use pruning/cleanup operations in bounded points (lock create/check/find) to avoid unbounded growth.
  - Maintain bounded audit/session query size and stable sort limits.

## 4) Suggested Delivery Milestones

### Milestone M1 (Week 1-2) - Security + Correctness

- Deliver Gate A + Gate B.

### Milestone M2 (Week 3-4) - Operability + Packaging

- Deliver Gate C and deployment package.

### Milestone M3 (Week 5-6) - Interop + Stability

- Deliver Gate D and publish customer-facing known limits.

### Milestone M4 (After first pilots) - SMB Beta

- Deliver Gate E with explicit beta policy.

## 5) Acceptance Commands (Baseline)

Keep this as a minimum CI/dev acceptance command set:

```powershell
cmake --build build --target test_base64 test_nas_core test_nas_service test_nas_redis_e2e test_nas_webdav_integration test_webdav test_http_features -j 4
ctest --test-dir build -R "base64|nas_core|nas_service|nas_redis_e2e|nas_webdav_integration|webdav|http_features" --output-on-failure
```

Add new suites as they are built:

- `nas_security` (traversal/auth/size limits)
- `nas_soak` (long-running large file workflows)
- `nas_concurrency` (parallel PUT/MOVE/DELETE/LOCK)
- `nas_recovery` (restart/crash cleanup)

## 6) Deployment Docs

- Private deployment runbook: `docs/server/NAS_DEPLOYMENT_RUNBOOK.md`
