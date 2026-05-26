# NAS Deployment Runbook (WebDAV-First)

Last updated: 2026-05-11

This runbook is for private deployment and pilot delivery.

## 1) Preflight

- Build `release_nas_server` in release mode.
- Prepare CA files if HTTPS is enabled.
- Prepare share directories with correct filesystem permissions.
- Prepare Redis endpoint for metadata/audit/session persistence.

## 2) Minimal Config Checklist

- Set `nas.webdav_mount` (default `/dav`).
- Set top-level `production_mode=true` for external/pilot deployment.
- Optionally set `nas.admin_console_path` (relative paths are resolved against config directory).
- Configure at least one NAS admin user.
- Use `pbkdf2-sha256$iterations$salt$hex_digest` for `password_hash`.
- Configure at least one share with an explicit absolute root path. The release template supports `root_path` for Linux/Unix, `windows_root_path` for Windows, and `root_env` (for example `YUAN_NAS_SHARE_ROOT`) as an environment override.
- Keep `allow_anonymous_read=false` for production.
- Keep `nas.redis.enabled=true` for production metadata/session/audit persistence.
- Keep SMB disabled for WebDAV-first rollout unless explicitly required.

Reference template:

- `docs/server/NAS_SERVICE_CONFIG_EXAMPLE.json`
- Release helper:
  - `release/nas/init_config.ps1` generates `pbkdf2-sha256` admin hashes and writes a production config.
  - `release/nas/generate_password_hash.ps1` prints a standalone hash for manual config edits.

## 3) Startup Sequence

1. Start NAS service with config:
   - `release_nas_server --config /etc/yuan/nas_service.json`
2. Verify `/nas/health` returns 200.
3. Verify `metadata_available`, `share_count`, and SMB status fields.
4. Verify `/nas/admin/readiness` returns `ready=true`, `production_mode=true`, and no blockers.
5. Verify authenticated WebDAV PUT/GET against `/dav/{share}`.

On Windows release packages, set these variables before `gate.ps1` to make the
gate verify authenticated readiness:

```powershell
$env:YUAN_NAS_CONFIG='C:\path\to\config.production.json'
$env:YUAN_NAS_ADMIN_USER='admin'
$env:YUAN_NAS_ADMIN_PASSWORD='<admin-password>'
$env:YUAN_NAS_GATE_REQUIRE_READINESS='1'
powershell -ExecutionPolicy Bypass -File .\gate.ps1
```

Startup is fail-closed when `production_mode=true`: weak password hashes,
anonymous read, relative share roots, and unsigned SMB beta exposure block
service initialization instead of relying only on post-start readiness checks.

## 4) Operational Checks

- Health endpoint:
  - `GET /nas/health`
- Admin endpoints (auth required):
  - `/nas/admin/users`
  - `/nas/admin/shares`
  - `/nas/admin/quota`
  - `/nas/admin/activity`
  - `/nas/admin/sessions`
  - `/nas/admin/audit`
- Admin console:
  - `/nas/admin`
  - Default resource file: `server/services/resources/nas_admin_console.html`
  - Recommended production config: set explicit `nas.admin_console_path` in config JSON.

## 5) Security Baseline

- Do not use `plain:` credentials in production.
- Keep legacy `fnv1a64$...` only during migration window; `production_mode=true` blocks weak/legacy hashes.
- Enforce HTTPS and trusted cert chain for internet-exposed deployment.
- Restrict admin endpoint access by network policy where possible.

## 6) Upgrade / Rollback

- Backup:
  - config file
  - Redis metadata (if enabled)
  - audit files
- Upgrade:
  1. stop service
  2. deploy new binary/config
  3. start service
  4. run health + WebDAV smoke
- Rollback:
  1. stop service
  2. restore previous binary/config
  3. restore metadata snapshot if schema changed
  4. start and verify

## 7) Acceptance Commands

```powershell
cmake --build build --target release_nas_server test_nas_core test_nas_service test_nas_concurrency test_nas_smb_adapter -j 4
ctest --test-dir build -R "nas_core|nas_service|nas_concurrency|nas_smb_adapter" --output-on-failure
```

For a fuller local gate, run:

```powershell
.\test\nas\scripts\nas_sell_preflight.ps1
```
