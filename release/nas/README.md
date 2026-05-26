# release/nas

This directory contains release-oriented runnable assets for the NAS service.

## Binary

- `release_nas_server`: NAS service entry backed by `NasService`

Default build output:

- Linux/MinGW: `build*/release/nas/release_nas_server`
- Visual Studio: `build*/release/nas/<Config>/release_nas_server.exe`

## Server Config

Default config file:

- `release/nas/config.json`

Key fields:

- `production_mode` (enables fail-closed startup checks)
- `port` (HTTP admin/API port)
- `http` (`thread_pool_size`, `enable_keep_alive`, etc.)
- `nas` (`webdav_mount`, `shares`, `users`, `audit`, `redis`)
- `smb` (`enabled`, `port`, signing/encryption options)

Environment override:

- `YUAN_NAS_CONFIG`

Server options:

```bash
build/release/nas/release_nas_server --config release/nas/config.json
build/release/nas/release_nas_server --config release/nas/config.json --workers 4
build/release/nas/release_nas_server -f release/nas/config.json
```

## Quick Run

The default release config is production-oriented and expects Redis at `127.0.0.1:6379`.

Initialize a production config with a real admin password:

```powershell
powershell -ExecutionPolicy Bypass -File release\nas\init_config.ps1 `
  -OutputPath release\nas\config.production.json `
  -AdminUser admin `
  -ShareRoot C:\yuan\nas\public
```

```bash
build/release/nas/release_nas_server --config release/nas/config.json
```

Or use helper scripts after building:

```bash
bash release/nas/start.sh
bash release/nas/stop.sh
```

Health check:

```bash
bash release/nas/health_check.sh
```

Gate:

```bash
bash release/nas/gate.sh
```

Windows helpers:

```powershell
powershell -ExecutionPolicy Bypass -File release\nas\generate_password_hash.ps1
powershell -ExecutionPolicy Bypass -File release\nas\start.ps1
powershell -ExecutionPolicy Bypass -File release\nas\health_check.ps1
powershell -ExecutionPolicy Bypass -File release\nas\stop.ps1
powershell -ExecutionPolicy Bypass -File release\nas\gate.ps1
```

Default endpoints (from example config):

- Admin and API: `http://127.0.0.1:8080/nas/admin`
- WebDAV mount: `http://127.0.0.1:8080/dav`
- Client guide: `CLIENTS.md`

## Notes

- `release_nas_server` uses `NasService` runtime behavior directly (including optional SMB sidecar when `smb.enabled=true`).
- `production_mode=true` refuses startup when readiness has blockers such as missing metadata, weak password hashes, relative share paths, or insecure SMB signing settings.
- The sample admin password hash is only a placeholder; replace it before exposing the service.
- The sample share root is `/srv/yuan/nas/public` on Linux/Unix and `C:/yuan/nas/public` on Windows. Set `YUAN_NAS_SHARE_ROOT` to override it, then create that directory and set OS-level permissions before serving users.
- Config paths are resolved relative to the config file directory when relative paths are provided.
- `gate.sh` and `gate.ps1` skip with exit code `77` when Redis is unavailable.
- Set `YUAN_NAS_ADMIN_USER`, `YUAN_NAS_ADMIN_PASSWORD`, and optionally `YUAN_NAS_GATE_REQUIRE_READINESS=1` to make gate scripts verify `/nas/admin/readiness`.
