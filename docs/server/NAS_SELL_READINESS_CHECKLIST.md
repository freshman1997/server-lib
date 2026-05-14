# NAS Sell Readiness Checklist

Last updated: 2026-05-12

Use this checklist before external paid rollout.

## A) Must Pass In Current Environment

- [ ] Build and run NAS core regression:
  - `ctest --test-dir build -R "nas_core|nas_service|nas_webdav_integration|nas_concurrency|nas_smb_adapter" --output-on-failure`
- [ ] Health and admin endpoints return expected status and payload fields.
- [ ] WebDAV authenticated upload/download works for configured share.
- [ ] Admin audit/session retention behavior is validated.
- [ ] Admin console is reachable at `/nas/admin` and serves expected HTML.
- [ ] Config includes `nas.admin_console_path` or default resource is present.

## B) Must Pass Before Selling (Can Be Deferred Locally)

- [ ] SMB external interop with `smbclient` (normal mode): upload/download/rename/delete/list.
- [ ] SMB external interop with `smbclient` (signing-required mode).
- [ ] SMB external result report archived with command outputs and environment details.
- [ ] WebDAV interop report archived for: rclone, cadaver, Windows WebClient, macOS Finder.
- [ ] TLS/certificate ops checklist complete (cert deploy, rotation, expiration handling, CA path policy).
- [ ] Packaging verification complete (fresh install, upgrade, rollback, config migration).

## C) Current Blocking Policy

If `smbclient` is unavailable in the current environment:

- Keep local NAS/SMB protocol and adapter tests green.
- Mark SMB external interop as `BLOCKED-ENV`.
- Do not mark release as sell-ready until section B SMB checks are completed in a tooling-ready environment.

## D) Suggested Status Labels

- `PASS`: validated and evidence archived.
- `BLOCKED-ENV`: not executable in current environment (tool missing).
- `FAIL`: executed but not passing.
- `N/A`: intentionally out of scope for this release.
