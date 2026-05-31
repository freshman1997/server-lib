# NAS Client Guide

Default endpoints:

- Admin console: `http://127.0.0.1:8080/nas/admin`
- Browser file console: `http://127.0.0.1:8080/nas/files`
- WebDAV: `http://127.0.0.1:8080/dav`
- Health: `http://127.0.0.1:8080/nas/health`

## Browser File Console

Use the browser file console for normal users who do not need admin features:

```text
http://127.0.0.1:8080/nas/files
```

Login with a NAS username and password created by an admin. The default file
path is:

```text
/dav/public/
```

Supported actions:

- Browse folders, including Chinese and nested paths
- Upload files
- Download files
- Delete files
- Preview images, PDF, text/code, audio, and video in a dialog

Direct browser visits to file URLs such as `/dav/public/report.pdf` redirect to
`/nas/files?path=/dav/public/report.pdf` when not logged in. After login, the
file opens in the preview dialog. WebDAV clients still receive standard `401`
responses and are not redirected.

## WebDAV

Use WebDAV as the default production access path.

For browser-only users, open `http://127.0.0.1:8080/nas/files`, enter the NAS
username and password, and use `/dav/public/` as the default file path. This
page does not expose admin APIs.

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

Windows File Explorer WebDAV:

1. Open File Explorer.
2. Right click `This PC`.
3. Choose `Add a network location`.
4. Use `http://host:8080/dav/public/`.
5. Enter the NAS username and password.

Linux `davfs2` example:

```bash
sudo mount -t davfs http://host:8080/dav/public/ /mnt/yuan-nas
```

## SMB

SMB is optional and disabled in the release config by default.

`net share` is a Windows OS share command and is not part of Yuan NAS. Use it
only when you intentionally want Windows itself to publish a local directory.
Yuan NAS SMB is the optional service enabled by the `smb` block below.

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

SMB listens on TCP `445` by default. Windows File Explorer cannot specify a
custom SMB port in `\\host\share` paths. If TCP `445` is already used by the
Windows host, run Yuan NAS SMB on Linux, a VM, container, or another machine.

SMB users need an NT hash in addition to the WebDAV/Admin PBKDF2 password hash.
Create one through the admin API by sending `smb_enabled=true` to reuse the same
password for SMB, or send `smb_password` to use a separate SMB password:

```bash
curl -u admin:<admin-password> -H 'Content-Type: application/json' \
  -d '{"id":"user-2","username":"zhangsan","password":"web-password","smb_enabled":true,"enabled":true,"admin":false}' \
  http://127.0.0.1:8080/nas/admin/users
```

The server stores `password_hash` for WebDAV/Admin and `smb_password_hash` for
SMB/NTLM.

PowerShell example:

```powershell
curl.exe -u admin:<admin-password> `
  -H "Content-Type: application/json" `
  -d "{\"id\":\"user-2\",\"username\":\"zhangsan\",\"password\":\"web-password\",\"smb_enabled\":true,\"enabled\":true,\"admin\":false}" `
  http://127.0.0.1:8080/nas/admin/users
```

Use a separate SMB password:

```powershell
curl.exe -u admin:<admin-password> `
  -H "Content-Type: application/json" `
  -d "{\"id\":\"user-3\",\"username\":\"lisi\",\"password\":\"web-password\",\"smb_password\":\"smb-password\",\"enabled\":true,\"admin\":false}" `
  http://127.0.0.1:8080/nas/admin/users
```

Map from Windows File Explorer:

```text
\\host\public
```

Map from Windows command line:

```powershell
net use Z: \\host\public /user:zhangsan <smb-password> /persistent:yes
```

Linux `smbclient`:

```bash
smbclient //host/public -U zhangsan
```

Linux CIFS mount:

```bash
sudo mount -t cifs //host/public /mnt/yuan-nas -o username=zhangsan,vers=3.0
```

SMB file names and paths are UTF-16 on the wire. Yuan NAS stores internal paths
as UTF-8 and supports Chinese file and directory names on Windows and Linux.

## Creating Users

Users are managed by the admin console or admin API.

Admin console:

1. Open `http://127.0.0.1:8080/nas/admin`.
2. Fill `Credentials` with an admin account and click `Save`.
3. In `Create User`, fill `id`, `username`, and `password`.
4. Check `smb` if the user should also log in over SMB.
5. Fill `SMB password` only when SMB should use a different password.
6. Click `Save User`.

API fields:

- `id`: stable user id, for example `user-zhangsan`
- `username`: login name
- `password`: WebDAV/browser/Admin password
- `smb_enabled`: generate SMB credentials from `password`
- `smb_password`: generate SMB credentials from this separate password
- `enabled`: set to `true`
- `admin`: set to `true` only for administrators

Production password storage:

- `password_hash`: PBKDF2-SHA256, used by browser/WebDAV/Admin
- `smb_password_hash`: NT hash, used by SMB/NTLM

Do not use `plain:` passwords in production configs.

## Creating Shares

Use the admin console `Create Share` form or the admin API:

```powershell
curl.exe -u admin:<admin-password> `
  -H "Content-Type: application/json" `
  -d "{\"id\":\"share-media\",\"name\":\"media\",\"root_path\":\"D:/media\",\"readonly\":false,\"enabled\":true,\"default_permissions\":[\"read\",\"write\",\"remove\"]}" `
  http://127.0.0.1:8080/nas/admin/shares
```

The share name becomes the access path:

- Browser/WebDAV: `/dav/media/`
- SMB: `\\host\media`

## Share Permissions

Share permissions are enforced consistently for browser access, WebDAV, and SMB.
Admin users bypass share ACLs; regular users use `default_permissions` unless a
per-user rule is configured.

Admin console:

1. Open `http://127.0.0.1:8080/nas/admin`.
2. Save admin credentials in `Credentials`.
3. In `Share Permissions`, enter the share id/name and username.
4. Check `read`, `write`, `delete`, or `admin`.
5. Click `Save Permission`.

API:

```powershell
curl.exe -u admin:<admin-password> `
  -H "Content-Type: application/json" `
  -d "{\"share\":\"public\",\"username\":\"zhangsan\",\"permissions\":[\"read\",\"write\",\"remove\"]}" `
  http://127.0.0.1:8080/nas/admin/permissions
```

Download-only:

```powershell
curl.exe -u admin:<admin-password> `
  -H "Content-Type: application/json" `
  -d "{\"share\":\"public\",\"username\":\"lisi\",\"permissions\":[\"read\"]}" `
  http://127.0.0.1:8080/nas/admin/permissions
```

Return a user to the share default permissions:

```powershell
curl.exe -u admin:<admin-password> `
  -H "Content-Type: application/json" `
  -d "{\"share\":\"public\",\"username\":\"lisi\",\"clear\":true}" `
  http://127.0.0.1:8080/nas/admin/permissions
```

Config seed by user id:

```json
"subject_permissions": {
  "user-zhangsan": ["read", "write", "remove"],
  "user-lisi": ["read"]
}
```

## Admin API

Admin endpoints require HTTP Basic authentication:

- `GET /nas/admin/readiness`
- `/nas/admin/users`
- `/nas/admin/shares`
- `/nas/admin/permissions`
- `/nas/admin/quota`
- `/nas/admin/activity`
- `/nas/admin/sessions`
- `/nas/admin/audit`

Use `/nas/health` for unauthenticated process and dependency checks.
