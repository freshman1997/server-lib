# NAS Client Guide

Default endpoints:

- Admin console: `http://127.0.0.1:8080/nas/admin`
- WebDAV: `http://127.0.0.1:8080/dav`
- Health: `http://127.0.0.1:8080/nas/health`

## WebDAV

Use WebDAV as the default production access path.

Common clients:

- Windows: File Explorer network location, WinSCP, Cyberduck, RaiDrive, rclone
- macOS: Finder `Connect to Server`, Cyberduck, Transmit, rclone
- Linux: davfs2, cadaver, rclone, GNOME/KDE file managers

Example rclone remote:

```text
type = webdav
url = http://127.0.0.1:8080/dav
vendor = other
user = admin
pass = <rclone-obscured-password>
```

## SMB

SMB is optional and disabled in the release config by default.

Enable it only when the deployment needs native file sharing:

```json
{
  "smb": {
    "enabled": true,
    "port": 445,
    "require_signing": true,
    "enable_encryption": false
  }
}
```

Common clients:

- Windows: File Explorer `\\host\public`
- macOS: Finder `smb://host/public`
- Linux: `smbclient`, CIFS mount

For production, keep `require_signing=true`.

## Admin API

Admin endpoints require HTTP Basic authentication:

- `GET /nas/admin/readiness`
- `/nas/admin/users`
- `/nas/admin/shares`
- `/nas/admin/quota`
- `/nas/admin/activity`
- `/nas/admin/sessions`
- `/nas/admin/audit`

Use `/nas/health` for unauthenticated process and dependency checks.
