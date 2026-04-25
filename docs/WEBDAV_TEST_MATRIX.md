# WebDAV Test Matrix

This matrix tracks the WebDAV surface intended for NAS-style storage integration.

| Area | Feature | Coverage |
| --- | --- | --- |
| HTTP parsing | OPTIONS, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK, REPORT, ACL, SEARCH methods | `test_webdav` parser smoke |
| Class 1 | OPTIONS capability, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND depth 0/1 | backend unit coverage plus `nas_webdav_integration` real server coverage |
| Class 2 | Exclusive/shared write locks, depth handling, refresh, UNLOCK, lock discovery | `test_webdav` lock coverage plus `nas_webdav_integration` LOCK/423/UNLOCK |
| Class 3 | PROPPATCH dead properties, quota live properties, multistatus response | backend/XML coverage |
| Copy/move | Destination parsing, overwrite behavior, recursive collections | backend unit coverage plus `nas_webdav_integration` COPY/MOVE |
| Safety | Root confinement, parent conflict, locked/precondition responses | backend and handler paths |
| NAS hooks | Backend abstraction for quota, metadata, content, copy/move, collection listing | `WebDavResourceBackend` |
| NAS server fixture | Authenticated temp-share WebDAV over a local HTTP port | `nas_webdav_integration` |
| Large file response | WebDAV GET avoids backend whole-file buffering when a local path is available | `nas_webdav_integration` large GET |

Interop targets to add when the server fixture is stable:

| Client | Scenario |
| --- | --- |
| macOS Finder | mount, upload, rename, delete, lock refresh |
| Windows WebClient | OPTIONS/PROPFIND discovery, Office-style LOCK/UNLOCK |
| cadaver/litmus | RFC 4918 Class 1/2 regression suite |
| rclone | sync tree, overwrite false, large PUT |

NAS stabilization dependencies:

| Dependency | Required For |
| --- | --- |
| NAS auth service | Basic Auth, SMB/SFTP identity mapping |
| NAS share manager | `/dav/{share}/...` routing and root confinement |
| NAS permission service | read/write/delete/admin checks |
| Redis metadata store | users, shares, ACL, sessions, dead properties, locks |
| Streaming file IO | large GET done for local/NAS WebDAV responses; PUT still needs HTTP parser hook |
