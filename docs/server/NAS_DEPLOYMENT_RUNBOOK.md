# NAS Deployment Runbook (WebDAV-First)

Last updated: 2026-05-11

This runbook is for private deployment and pilot delivery.

## 1) Preflight

- Build binaries in release mode.
- Prepare CA files if HTTPS is enabled.
- Prepare share directories with correct filesystem permissions.
- Optional: prepare Redis endpoint for metadata/audit/session persistence.

## 2) Minimal Config Checklist

- Set `nas.webdav_mount` (default `/dav`).
- Optionally set `nas.admin_console_path` (relative paths are resolved against config directory).
- Configure at least one NAS admin user.
- Use `pbkdf2-sha256$iterations$salt$hex_digest` for `password_hash`.
- Configure at least one share with explicit root path.
- Keep `allow_anonymous_read=false` for production.
- Keep SMB disabled for WebDAV-first rollout unless explicitly required.

Reference template:

- `docs/server/NAS_SERVICE_CONFIG_EXAMPLE.json`

## 3) Startup Sequence

1. Start NAS service with config.
2. Verify `/nas/health` returns 200.
3. Verify `metadata_available`, `share_count`, and SMB status fields.
4. Verify authenticated WebDAV PUT/GET against `/dav/{share}`.

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
- Keep legacy `fnv1a64$...` only during migration window.
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
cmake --build build --target NasCore ServerServices test_nas_core test_nas_smb_adapter -j 4
ctest --test-dir build -R "nas_core|nas_smb_adapter" --output-on-failure
```

Note: `nas_service` smoke may depend on environment SSL/CA setup.
