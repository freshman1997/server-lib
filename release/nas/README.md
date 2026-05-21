# release/nas

This directory contains release-oriented runnable assets for NAS service.

## Binary

- `release_nas_server`: NAS service entry backed by `NasService`

Default build output:

- `build/release/nas/release_nas_server`

## Server Config

Default config file:

- `release/nas/config.json`

Key fields:

- `port` (HTTP admin/API port)
- `http` (`thread_pool_size`, `enable_keep_alive`, etc.)
- `nas` (`webdav_mount`, `shares`, `users`, `audit`, `redis`)
- `smb` (`enabled`, `port`, signing/encryption options)

Environment override:

- `YUAN_NAS_CONFIG`

Server options:

```bash
build/release/nas/release_nas_server --config release/nas/config.json
build/release/nas/release_nas_server -f release/nas/config.json
```

## Quick Run

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

Default endpoints (from example config):

- Admin and API: `http://127.0.0.1:8080/admin`
- WebDAV mount: `http://127.0.0.1:8080/dav`

## Notes

- `release_nas_server` uses `NasService` runtime behavior directly (including optional SMB sidecar when `smb.enabled=true`).
- Config paths are resolved relative to the config file directory when relative paths are provided.
