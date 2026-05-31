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

### 1. Build

Visual Studio:

```powershell
cmake -S . -B build-vs-nas-release -G "Visual Studio 17 2022" -A x64 -DYUAN_BUILD_TESTS=OFF -DYUAN_BUILD_EXAMPLE=OFF
cmake --build build-vs-nas-release --target release_nas_server --config Release -j 4
```

MinGW:

```powershell
cmake -S . -B build-mingw-nas-release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DYUAN_BUILD_TESTS=OFF -DYUAN_BUILD_EXAMPLE=OFF
cmake --build build-mingw-nas-release --target release_nas_server -j 4
```

Linux:

```bash
cmake -S . -B build-linux-nas-release -DCMAKE_BUILD_TYPE=Release -DYUAN_BUILD_TESTS=OFF -DYUAN_BUILD_EXAMPLE=OFF
cmake --build build-linux-nas-release --target release_nas_server -j 4
```

### 2. Initialize Config

Initialize a production config with a real admin password and share directory:

```powershell
powershell -ExecutionPolicy Bypass -File release\nas\init_config.ps1 `
  -OutputPath release\nas\config.production.json `
  -AdminUser admin `
  -ShareRoot D:\misc
```

The script creates the share directory, writes PBKDF2 admin credentials, enables
Redis metadata, and leaves SMB disabled until you explicitly enable it.

### 3. Start

Visual Studio output:

```powershell
.\build-vs-nas-release\release\nas\Release\release_nas_server.exe --config .\release\nas\config.production.json
```

MinGW output:

```powershell
.\build-mingw-nas-release\release\nas\release_nas_server.exe --config .\release\nas\config.production.json
```

Linux output:

```bash
./build-linux-nas-release/release/nas/release_nas_server --config ./release/nas/config.production.json
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

### 4. Access

- Admin and API: `http://127.0.0.1:8080/nas/admin`
- Browser file console: `http://127.0.0.1:8080/nas/files`
- WebDAV root: `http://127.0.0.1:8080/dav/public/`
- Health: `http://127.0.0.1:8080/nas/health`
- Client guide: `CLIENTS.md`

Open `/nas/admin` with the admin username and password from initialization.
Open `/nas/files` for normal file browsing, upload, download, preview, and delete.
Use `/dav/public/` when mounting with a WebDAV client.

If a browser opens a `/dav/public/...` file path without credentials, it is
redirected to `/nas/files?path=...`. After login, directories open in the file
console and files open in the preview dialog.

### 5. Create Users

Use the admin console `Create User` form or call the admin API.

WebDAV/browser user:

```powershell
curl.exe -u admin:<admin-password> `
  -H "Content-Type: application/json" `
  -d "{\"id\":\"user-lisi\",\"username\":\"lisi\",\"password\":\"UserPass123\",\"enabled\":true,\"admin\":false}" `
  http://127.0.0.1:8080/nas/admin/users
```

User with SMB enabled and the same password:

```powershell
curl.exe -u admin:<admin-password> `
  -H "Content-Type: application/json" `
  -d "{\"id\":\"user-zhangsan\",\"username\":\"zhangsan\",\"password\":\"UserPass123\",\"smb_enabled\":true,\"enabled\":true,\"admin\":false}" `
  http://127.0.0.1:8080/nas/admin/users
```

User with separate WebDAV and SMB passwords:

```powershell
curl.exe -u admin:<admin-password> `
  -H "Content-Type: application/json" `
  -d "{\"id\":\"user-wangwu\",\"username\":\"wangwu\",\"password\":\"WebPass123\",\"smb_password\":\"SmbPass123\",\"enabled\":true,\"admin\":false}" `
  http://127.0.0.1:8080/nas/admin/users
```

`password` is stored as `password_hash` for WebDAV/Admin. `smb_enabled=true` or
`smb_password` generates `smb_password_hash` for SMB/NTLM.

### 6. Grant Share Permissions

Share `default_permissions` apply to authenticated non-admin users unless a
per-user rule exists. Admin users bypass share ACLs.

Use the admin console `Share Permissions` form, or call:

```powershell
curl.exe -u admin:<admin-password> `
  -H "Content-Type: application/json" `
  -d "{\"share\":\"public\",\"username\":\"lisi\",\"permissions\":[\"read\",\"write\",\"remove\"]}" `
  http://127.0.0.1:8080/nas/admin/permissions
```

Use only `["read"]` for download-only users. The same permissions are enforced
by the browser file console, WebDAV clients, and SMB.

Clear a per-user rule and return to the share default:

```powershell
curl.exe -u admin:<admin-password> `
  -H "Content-Type: application/json" `
  -d "{\"share\":\"public\",\"username\":\"lisi\",\"clear\":true}" `
  http://127.0.0.1:8080/nas/admin/permissions
```

Config files can also seed ACLs by user id:

```json
"subject_permissions": {
  "user-lisi": ["read", "write", "remove"]
}
```

### 7. Enable SMB

SMB is disabled in the release config by default. Enable it only when the host
can expose the SMB port:

```json
"smb": {
  "enabled": true,
  "port": 445,
  "require_signing": true,
  "enable_encryption": false,
  "server_name": "YUAN-NAS",
  "domain_name": "WORKGROUP"
}
```

Windows File Explorer uses TCP `445` and cannot map `\\host:1445\public`. If
Windows itself already owns port `445`, run Yuan NAS on Linux, a VM, container,
or another machine for native Windows SMB mapping.

SMB client paths:

- Windows File Explorer: `\\host\public`
- macOS Finder: `smb://host/public`
- Linux: `smbclient //host/public -U zhangsan`

## Notes

- `release_nas_server` uses `NasService` runtime behavior directly (including optional SMB sidecar when `smb.enabled=true`).
- `production_mode=true` refuses startup when readiness has blockers such as missing metadata, weak password hashes, relative share paths, or insecure SMB signing settings.
- The sample admin password hash is only a placeholder; replace it before exposing the service.
- The sample share root is `/srv/yuan/nas/public` on Linux/Unix and `C:/yuan/nas/public` on Windows. Set `YUAN_NAS_SHARE_ROOT` to override it, then create that directory and set OS-level permissions before serving users.
- Users and shares created through the admin API are stored in Redis metadata. Enable Redis persistence for production or keep equivalent bootstrap users and shares in the config.
- Config paths are resolved relative to the config file directory when relative paths are provided.
- `gate.sh` and `gate.ps1` skip with exit code `77` when Redis is unavailable.
- Set `YUAN_NAS_ADMIN_USER`, `YUAN_NAS_ADMIN_PASSWORD`, and optionally `YUAN_NAS_GATE_REQUIRE_READINESS=1` to make gate scripts verify `/nas/admin/readiness`.
