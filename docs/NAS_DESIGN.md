# Yuan NAS Design

This document turns the current protocol framework into a staged NAS server plan. The first stable target is a WebDAV-based NAS MVP, then shared NAS core services, then SMB/SFTP reuse.

## Goals

- Provide file access through WebDAV first, then SMB and SFTP.
- Keep storage backends modular so local disk can later become virtual volumes, object storage, snapshots, or distributed storage.
- Use Redis as the temporary metadata DB for users, shares, locks, sessions, dead properties, audit markers, and background task state.
- Keep large file data on the filesystem, not in Redis.
- Make every phase verifiable through an automated matrix plus a small set of real client interop tests.

## Current Base

Already available:

- HTTP runtime and `HttpService`.
- WebDAV protocol module with handler/backend split.
- `LocalWebDavBackend` for local filesystem access.
- WebDAV method parsing in HTTP/1.1 and HTTP/2 dispatch.
- SMB server/share/filesystem/auth structure.
- SSH SFTP filesystem abstraction.
- Redis client library and plugin Redis storage wrapper.
- CTest coverage for WebDAV and HTTP features.

Important gaps:

- No unified NAS account/permission model yet.
- WebDAV file transfer currently uses in-memory body paths, not streaming.
- WebDAV locks and dead properties need Redis persistence.
- No NAS service wrapper or config loader yet.
- SMB/SFTP need shared storage/auth adapters and real client interop.

## Architecture

```text
NAS Service
  |
  +-- NasConfig
  +-- NasAuthService
  +-- NasShareManager
  +-- NasPermissionService
  +-- NasMetadataStore
  +-- NasStorageBackend
  +-- NasAuditLog
  |
  +-- Protocol adapters
      +-- WebDAV adapter
      +-- SMB adapter
      +-- SFTP adapter
      +-- HTTP admin API
```

## Core Abstractions

`NasStorageBackend`

- `stat(path)`
- `list(path)`
- `open_read(path, range)`
- `open_write(path, mode)`
- `mkdir(path)`
- `remove(path)`
- `copy(from, to, overwrite)`
- `move(from, to, overwrite)`
- `quota(share/user)`

`NasMetadataStore`

- User records.
- Share records.
- ACL records.
- WebDAV dead properties.
- WebDAV lock records.
- Session tokens.
- Audit stream cursors.
- Background job state.

`NasAuthService`

- Password verification.
- Token/session verification.
- Protocol identity mapping.
- Rate limiting hooks.

Current implementation note:

- Basic Auth parsing supports `pbkdf2-sha256$iterations$salt$hex_digest` as the default password hash format.
- Legacy `fnv1a64$...` and `plain:...` are kept only for migration/compatibility and should be disabled by production policy.

`NasPermissionService`

- Share-level ACL.
- Path-level ACL.
- Method/action mapping:
  - `read`: GET, HEAD, PROPFIND, SMB read, SFTP read
  - `write`: PUT, PROPPATCH, SMB write, SFTP write
  - `delete`: DELETE, SMB delete, SFTP remove
  - `admin`: share/config management

## Redis Plan

Redis is metadata only. File content stays on disk.

Key prefix:

```text
yuan:nas:
```

Initial schema:

```text
yuan:nas:user:{user_id}                  HASH username,password_hash,status,created_at,updated_at
yuan:nas:user_by_name:{username}         STRING user_id
yuan:nas:share:{share_id}                HASH name,path,enabled,readonly,created_at,updated_at
yuan:nas:share_by_name:{name}            STRING share_id
yuan:nas:acl:{share_id}                  HASH subject -> permission mask
yuan:nas:session:{token}                 HASH user_id,created_at,last_seen,expires_at
yuan:nas:webdav:prop:{share_id}:{path}   HASH property_name -> value
yuan:nas:webdav:lock:{token}             HASH share_id,path,scope,depth,owner,expires_at
yuan:nas:audit                          STREAM event records
yuan:nas:job:{job_id}                    HASH type,status,payload,created_at,updated_at
```

Temporary constraints:

- Redis unavailability must not corrupt local file data.
- Writes requiring metadata, such as locks or ACL checks, should fail closed.
- Read-only unauthenticated mode may be allowed only behind explicit config.

## WebDAV MVP Behavior

Required before calling WebDAV stable:

- Basic authentication.
- Share mount path support, such as `/dav/{share}/...`.
- ACL checks on every mutating method.
- Streaming GET and PUT.
- Redis-persisted locks.
- Redis-persisted dead properties.
- Real client interop:
  - rclone
  - cadaver
  - Windows WebClient
  - macOS Finder

## Non-Goals For MVP

- Distributed storage.
- RAID management.
- Snapshot UI.
- Media indexing.
- Full SMB production compatibility.
- Full WebDAV ACL RFC 3744.

These can come after the WebDAV MVP is stable.

## Risks

- Existing Redis storage wrapper has a hardcoded host in plugin code. NAS should use a new config-driven Redis metadata store, not reuse the plugin wrapper as-is.
- HTTP request body buffering limits large PUT; streaming must be addressed early.
- Windows WebDAV clients are strict about lock/PROPFIND behavior.
- SMB is protocol-heavy; treat it as a second-stage compatibility effort, not the first stable path.
