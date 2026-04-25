# SMB NAS Progress

Last updated: 2026-04-25

This document records the current SMB/NAS integration state so future work can continue from the same point without rediscovering context.

## Current Goal

Move the in-repo SMB implementation toward practical Samba `smbd` compatibility for NAS use:

- Reuse NAS users, shares, and ACL decisions.
- Support common Linux `smbclient`, Linux CIFS, Windows Explorer, and macOS Finder file workflows.
- Avoid advertising SMB capabilities that are not fully implemented.
- Prefer small, testable compatibility increments over large speculative protocol work.

## Recently Completed

### NAS SMB Adapter

- Added NAS-to-SMB share config generation from NAS shares.
- Added `NasSmbHandler` hooks for SMB auth, tree connect, create, read, write, query directory, query info, set info, logoff, and session cleanup.
- Mapped SMB operations to NAS permissions:
  - tree/query/read paths require read permission
  - write/create/truncate paths require write permission
  - delete-on-close / delete disposition paths require delete permission
- Added `test_nas_smb_adapter`.

### Authentication

- Wired SMB SPNEGO/NTLM flow to handler password lookup.
- Added NTLMv2 password-proof tests.
- Current production gap: NAS password hash format is not fully integrated for SMB NTLMv2; `plain:` development credentials are supported for the adapter flow.

### Local SMB Filesystem Behavior

- Added directory opening on POSIX.
- Added path confinement fixes for SMB-style root-relative paths such as `\new.txt`.
- Added `FileRenameInformation`, `FileDispositionInformation`, and `FileEndOfFileInformation` behavior.
- Added per-handle directory enumeration cursor.
- Added common directory response encodings:
  - `FileDirectoryInformation`
  - `FileFullDirectoryInformation`
  - `FileBothDirectoryInformation`
  - `FileIdFullDirectoryInformation`
  - `FileIdBothDirectoryInformation`

### smbd Compatibility Increments

- Conservative SMB capability advertisement:
  - do not advertise multi-channel, persistent handles, or directory leasing before full support
  - only advertise encryption when `enable_encryption` is enabled
- Added filesystem query info responses:
  - `FileFsVolumeInformation`
  - `FileFsSizeInformation`
  - `FileFsFullSizeInformation`
  - `FileFsDeviceInformation`
  - `FileFsAttributeInformation`
  - `FileFsSectorSizeInformation`
- Added high-frequency FSCTL handling:
  - `FSCTL_VALIDATE_NEGOTIATE_INFO`
  - `FSCTL_QUERY_NETWORK_INTERFACE_INFO`
  - `FSCTL_LMR_REQUEST_RESILIENCY`
  - existing DFS referral and pipe transceive paths remain
- Changed `CHANGE_NOTIFY` from never-completed `PENDING` to a legal empty completion response as a temporary compatibility fallback.
- Added request signing verification and `require_signing` rejection for unsigned post-auth requests.
- Ensured signed responses set `SMB2_FLAGS_SIGNED` and compute the signature over a zeroed signature field.

### Interop Support

- Added optional `smbclient_nas_smoke.sh` workflow for:
  - upload
  - download
  - rename
  - delete
  - list
- Current gap: the script still needs an automated local server fixture before it becomes a reliable CI test.

## Validation Snapshot

Known passing commands after the latest SMB work:

```bash
cmake --build build --target test_smb -j2
./build/test/protocol/smb/test_smb
```

Expected result:

```text
48/48 passed, 0 failed
```

NAS adapter validation:

```bash
cmake --build build --target test_nas_smb_adapter -j2
./build/test/nas/test_nas_smb_adapter
```

Expected result:

```text
NAS SMB adapter tests passed
```

`ctest` registration is incomplete in the current build state; direct test binaries are the reliable acceptance path for now.

## Current State By Area

| Area | State | Notes |
| --- | --- | --- |
| SMB2/SMB3 negotiate | usable | Conservative capabilities now better match implemented behavior. |
| Session setup | partial | NTLMv2 unit path works; production NAS hash integration and client login testing remain. |
| Tree connect | usable | NAS share mapping and IPC$ are available. |
| File create/read/write/close | usable | Basic disk share workflows are covered by unit tests. |
| Directory listing | usable | Common info classes and cursor behavior are covered. |
| Rename/delete/truncate | usable | SetInfo-backed local filesystem behavior is covered. |
| Filesystem info | usable | Common client mount queries are covered. |
| IOCTL/FSCTL | partial | Common mount-path FSCTLs are covered; copychunk, snapshots, and richer DFS/RPC remain. |
| Change notify | fallback | Empty completion avoids stuck clients; real async notification remains. |
| Signing | partial | Basic verify/sign policy exists; real `smbclient --signing=required` interop remains. |
| Encryption | partial | Crypto primitives exist; end-to-end SMB3 encryption interop remains. |
| Durable handles | missing | Needs create-context and reconnect behavior. |
| Multi-channel | missing | Capability is intentionally not advertised. |

## Next Implementation Order

1. Add an automated local SMB/NAS server fixture for `smbclient_nas_smoke.sh`.
2. Run and document Linux `smbclient` interop with normal and signing-required modes.
3. Replace empty `CHANGE_NOTIFY` with real async directory notifications.
4. Integrate production NAS password hashes into NTLMv2 verification.
5. Add Linux CIFS mount smoke coverage.
6. Add Windows Explorer and macOS Finder manual test notes.
7. Add durable handle create contexts and reconnect behavior.
8. Validate SMB3 encryption against real clients before enabling it by default.

## Related Documents

- `docs/SMB_SMBD_COMPAT_MATRIX.md`
- `docs/SMB_DESIGN.md`
- `docs/NAS_TASK_BREAKDOWN.md`
- `docs/NAS_DESIGN.md`
