# Yuan NAS Task Breakdown

This is the working task list for turning the current framework into a stable NAS server. Keep each milestone shippable and testable.

## Phase 0 - Baseline And Documentation

Status: complete

| ID | Task | Output | Validation |
| --- | --- | --- | --- |
| N0.1 | Land NAS design document | `docs/NAS_DESIGN.md` | done |
| N0.2 | Land task breakdown | `docs/NAS_TASK_BREAKDOWN.md` | done |
| N0.3 | Keep WebDAV matrix current | `docs/WEBDAV_TEST_MATRIX.md` | done |
| N0.4 | Verify baseline tests | `webdav`, `http_features` | done |

## Phase 1 - WebDAV NAS MVP

Goal: a usable single-user or small-team NAS over WebDAV.

| ID | Task | Output | Validation |
| --- | --- | --- | --- |
| N1.1 | Add NAS config model | Config structs for Redis, shares, auth, WebDAV | done: `nas_core` |
| N1.2 | Add Redis metadata store | `NasRedisMetadataStore` using `libs/redis_cli` | done: key/schema tests and optional Redis e2e |
| N1.3 | Add user/auth model | user CRUD, password hash verify, Basic Auth middleware | done: auth service and HTTP middleware tests |
| N1.4 | Add share manager | share lookup by URL, root path confinement | done: URL route/share resolution tests |
| N1.5 | Add permission service | read/write/delete/admin action checks | done: ACL and WebDAV method mapping tests |
| N1.6 | Wire WebDAV to NAS auth/share/ACL | WebDAV adapter uses NAS services | done: mount helper and share-aware backend tests |
| N1.7 | Persist WebDAV dead properties in Redis | replace local `.webdav-properties` for NAS mode | done: NAS backend Redis e2e |
| N1.8 | Persist WebDAV locks in Redis | lock survives handler instance restart | done: Redis lock manager e2e and mount wiring |
| N1.9 | Stream GET/PUT | no full file buffering for large files | done: GET uses HTTP file task; large HTTP/1 PUT spools body to temp file and WebDAV writes from file |
| N1.10 | Add WebDAV server fixture | local port fixture with temp share | done: PUT/PROPFIND/MKCOL/COPY/MOVE/LOCK/UNLOCK/DELETE/auth integration |
| N1.11 | Run client interop | rclone/cadaver/Windows/macOS | manual/optional scripts |

## Phase 2 - NAS Service Productization

Goal: a configurable service that can be started as part of the app/server layer.

| ID | Task | Output | Validation |
| --- | --- | --- | --- |
| N2.1 | Add `NasService` | server service wrapper that mounts WebDAV | done: `nas_service` smoke |
| N2.2 | Add config file | JSON config for redis, shares, auth, ports | done: JSON load + service reload smoke |
| N2.3 | Add admin HTTP API | users, shares, quota, activity, sessions | done: users/shares list/upsert, quota, activity, audit, and sessions |
| N2.4 | Add audit log | Redis-backed event history plus file fallback | done: Redis audit, file fallback, API, admin view, and service tests |
| N2.5 | Add operational health endpoint | Redis, shares, disk space, protocol status | done: `/nas/health` service smoke |
| N2.6 | Add startup bootstrap | create admin/share if empty | boot tests |
| N2.7 | Add admin console backend | browser UI shell backed by admin APIs | in progress: health, health actions, users/shares forms, quota, activity, sessions, and audit views done |

## Phase 3 - Multi-Protocol Reuse

Goal: SMB/SFTP use the same NAS identity, share, permission, and storage model.

| ID | Task | Output | Validation |
| --- | --- | --- | --- |
| N3.1 | Adapt SFTP to `NasStorageBackend` | shared filesystem behavior | OpenSSH SFTP tests |
| N3.2 | Adapt SMB share manager to NAS shares | done: SMB share config generated from NAS config/metadata | adapter unit tests; `smbclient` tests still needed |
| N3.3 | Map SMB auth to NAS users | in progress: SMB handler validates enabled NAS users and NTLMv2 proof; `pbkdf2-sha256` is now default for HTTP/WebDAV auth and is intentionally not exposed to SMB password lookup | NTLMv2 and adapter unit tests; Windows/Linux login tests still needed |
| N3.4 | Map SMB permissions to NAS ACL | done: SMB tree/create/read/write/query/set-info checks use NAS ACL | adapter unit tests; real SMB ACL tests still needed |
| N3.5 | Add SMB interop matrix | in progress: optional `smbclient_nas_smoke` script covers list/upload/download/rename/delete; see `docs/SMB_SMBD_COMPAT_MATRIX.md` and `docs/SMB_NAS_PROGRESS.md` | Windows/macOS/Linux CIFS matrix still needed |

## Phase 4 - Stability

Goal: sustained use without data loss or resource leaks.

| ID | Task | Output | Validation |
| --- | --- | --- | --- |
| N4.1 | Soak WebDAV upload/download | long-running rclone sync | soak script |
| N4.2 | Crash recovery test | locks/session cleanup, temp files | recovery tests |
| N4.3 | Concurrency tests | parallel PUT/MOVE/DELETE/LOCK | stress test |
| N4.4 | Security pass | traversal, auth bypass, header limits | security tests |
| N4.5 | Performance profile | memory use and throughput baseline | benchmark report |

## Immediate Next Steps

Work in this order:

1. Add an automated local SMB/NAS server fixture for `smbclient_nas_smoke.sh`.
2. Run and document Linux `smbclient` interop with normal and signing-required modes.
3. Replace empty SMB `CHANGE_NOTIFY` completion with real async directory notifications.
4. Add rclone/cadaver/manual WebDAV interop scripts.
5. Add concurrency and soak tests for large WebDAV workflows.

## Current Acceptance Command

```powershell
cmake --build build --target test_base64 test_nas_core test_nas_service test_nas_redis_e2e test_nas_webdav_integration test_webdav test_http_features -j 4
ctest --test-dir build -R "base64|nas_core|nas_service|nas_redis_e2e|nas_webdav_integration|webdav|http_features" --output-on-failure
```

## Commercialization Plan

See `docs/NAS_COMMERCIALIZATION_PLAN.md` for the production rollout gates, priority order, and milestone schedule focused on WebDAV-first private deployment.

## Active Refactor Rules (Commercialization)

- Keep security-critical code paths short, explicit, and unit-testable.
- Prefer interface-level extension over protocol-specific `dynamic_cast` in runtime paths.
- Add backward-compatible migration shims first, then add policy toggles to phase out legacy behavior.
- Any new feature must include one correctness test and one regression test.

## Recent Hardening Notes

- Admin API now enforces bounded JSON request body size.
- Admin list endpoints now support bounded `limit` query parameter.
- Admin API now includes per-actor+remote simple rate limiting and returns HTTP 429 on excess traffic.
