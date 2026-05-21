# bt_downloader

`bt_downloader` is a standalone BitTorrent download service with a built-in Web admin dashboard.

## Features

- Web dashboard: `/admin`
- Add tasks by uploading `.torrent`, by server-side `.torrent` path, or by magnet URI
- Multi-task queue with start, pause, resume, stop, remove, retry, and detail views
- Progress, peer, tracker, DHT, NAT, speed, ratio, and task history monitoring
- Persistent task list and uploaded torrent files
- Optional admin API token via `YUAN_ADMIN_TOKEN`
- Plain HTTP dashboard by default, with optional SSL via config or env

## Build

```bash
cmake --build build --target bt_downloader -j4
```

## Run

```bash
./build/release/bt_downloader/bt_downloader
```

Or use helper scripts after building:

```bash
bash release/bt_downloader/start.sh
bash release/bt_downloader/stop.sh
```

Health check:

```bash
bash release/bt_downloader/health_check.sh
```

Gate:

```bash
bash release/bt_downloader/gate.sh
```

Then open:

```text
http://127.0.0.1:18080/admin
```

## Configuration

Priority: environment variables > config file > defaults.

The default config file is:

```text
release/bt_downloader/config.json
```

You can also pass a config file path:

```bash
./build/release/bt_downloader/bt_downloader /path/to/config.json
```

or use:

```bash
YUAN_BT_CONFIG=/path/to/config.json ./build/release/bt_downloader/bt_downloader
```

## Environment Variables

- `YUAN_BT_CONFIG`: config file path
- `YUAN_BT_ADMIN_PORT`: dashboard/API port
- `YUAN_BT_TORRENT_FILE`: torrent file loaded on startup
- `YUAN_BT_SAVE_PATH`: default download directory
- `YUAN_BT_MAX_PEERS`: max peers per task
- `YUAN_BT_LISTEN_PORT`: first BT listen port
- `YUAN_BT_LISTEN_PORT_END`: last BT listen port
- `YUAN_BT_DOWNLOAD_LIMIT_KBPS`: download limit in KB/s, `0` means unlimited
- `YUAN_BT_UPLOAD_LIMIT_KBPS`: upload limit in KB/s, `0` means unlimited
- `YUAN_BT_ENABLE_DHT`: enable DHT, `1/true/on` or `0/false/off`
- `YUAN_BT_ENABLE_PEX`: enable PEX
- `YUAN_BT_ENABLE_UPNP`: enable UPnP/NAT-PMP
- `YUAN_BT_ENABLE_SSL`: enable HTTPS for the admin server
- `YUAN_BT_MAX_CONCURRENT`: max concurrent running downloads
- `YUAN_ADMIN_TOKEN`: require `Authorization: Bearer <token>` for admin APIs

## Admin API

- `GET /admin/api/overview`
- `GET /admin/api/bt/tasks`
- `GET /admin/api/bt/tasks/detail?task_id=1`
- `POST /admin/api/bt/torrent` with `{"torrent_path":"...","save_path":"..."}`
- `POST /admin/api/bt/magnet` with `{"magnet_uri":"...","save_path":"..."}`
- `POST /admin/api/bt/upload` multipart form field `torrent`
- `POST /admin/api/bt/tasks/start` with `{"task_id":1}`
- `POST /admin/api/bt/tasks/pause` with `{"task_id":1}`
- `POST /admin/api/bt/tasks/resume` with `{"task_id":1}`
- `POST /admin/api/bt/tasks/stop` with `{"task_id":1}`
- `POST /admin/api/bt/tasks/remove` with `{"task_id":1}`
- `POST /admin/api/bt/settings`
- `GET /admin/api/bt/history?page=1&page_size=20`
