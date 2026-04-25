# SMB smbd Compatibility Matrix

This matrix tracks the practical Linux Samba `smbd` behavior needed for NAS interoperability.

For chronological implementation notes and validation commands, see `docs/SMB_NAS_PROGRESS.md`.

## Step 1 - Client Mount Path

| Area | Status | Notes |
| --- | --- | --- |
| SMB2/SMB3 negotiate | in progress | Dialects supported; capabilities now avoid advertising unimplemented multi-channel, persistent handles, and directory leasing. |
| Session setup | in progress | SPNEGO/NTLMv2 works in unit tests; production NAS password hash integration still needs client interop. |
| Tree connect | done | Disk shares and IPC$ are available. |
| Query filesystem info | done | Supports volume, size, full size, device, attributes, and sector-size classes. |
| Common IOCTL | in progress | Supports validate negotiate info, query network interface info, LMR resiliency, DFS referral stub, and pipe transceive. |
| Change notify | partial | Completes with an empty response instead of leaving requests pending; real async notifications remain. |

## Step 2 - File Workflows

| Area | Status | Notes |
| --- | --- | --- |
| Create/open/close | in progress | Basic file and directory handles work; durable/persistent handles remain. |
| Read/write/flush | done | Local filesystem backend supports offset I/O and fsync. |
| Directory enumeration | done | Supports common directory info classes and per-handle cursors. |
| Rename/delete/truncate | done | SetInfo handles rename, disposition, and EOF. |
| Lock/oplock/lease | partial | Managers exist; break notification and conflict semantics need real client stress tests. |

## Step 3 - Security

| Area | Status | Notes |
| --- | --- | --- |
| Response signing | in progress | Responses are signed with the SMB2 signed flag when a signing key exists. |
| Request signing verify | in progress | Signed requests are verified; `require_signing` rejects unsigned post-auth requests. |
| SMB3 encryption | partial | Crypto primitives exist; client interoperability still needs validation. |
| Preauth integrity | partial | Key derivation exists; full transcript validation remains. |

## Step 4 - Interop Tests

| Client | Status | Target workflow |
| --- | --- | --- |
| `smbclient` | partial | Script exists for list, upload, download, rename, delete. Needs an automated server fixture. |
| Linux CIFS mount | missing | mount, list, copy, rename, delete, unmount. |
| Windows Explorer | missing | map network drive, browse, copy, rename, delete, refresh. |
| macOS Finder | missing | connect to server, browse, copy, rename, delete. |

## Next Implementation Order

1. Add an automated local SMB server fixture for `smbclient_nas_smoke.sh`.
2. Add request/response signing interop tests with `smbclient --signing=required` when available.
3. Replace empty `CHANGE_NOTIFY` completion with real asynchronous directory notifications.
4. Add durable handle create contexts and reconnect behavior.
5. Validate SMB3 encryption with Linux and Windows clients before enabling it by default.
