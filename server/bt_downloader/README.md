# bt_downloader

`bt_downloader` 是一个独立的 BT 下载服务，内置 Web 管理后台。

## 功能

- Web 后台：`/admin`
- BT 监控：下载进度、peer、事件计数
- BT 控制：启动/停止、加载 `.torrent` 文件
- 构建后自动拷贝 web 资源到可执行目录下 `web/admin`

## 构建

```bash
cmake --build build --target bt_downloader -j4
```

## 运行

```bash
./build/server/bt_downloader/bt_downloader
```

启动后访问：`http://127.0.0.1:18080/admin`

## 配置

优先级：环境变量 > 配置文件 > 默认值。

### 配置文件

默认读取：`server/bt_downloader/config.json`。

也可通过参数或环境变量指定：

```bash
./build/server/bt_downloader/bt_downloader /path/to/config.json
```

或

```bash
YUAN_BT_CONFIG=/path/to/config.json ./build/server/bt_downloader/bt_downloader
```

配置项示例见 `server/bt_downloader/config.json`。

### 环境变量

- `YUAN_BT_CONFIG`：配置文件路径
- `YUAN_BT_ADMIN_PORT`：后台端口
- `YUAN_BT_TORRENT_FILE`：启动时加载 torrent 文件
- `YUAN_BT_SAVE_PATH`：下载保存目录
- `YUAN_BT_MAX_PEERS`：最大 peer 数
- `YUAN_BT_LISTEN_PORT`：BT 监听端口
- `YUAN_BT_DOWNLOAD_LIMIT_KBPS`：下载限速（KB/s）
- `YUAN_BT_UPLOAD_LIMIT_KBPS`：上传限速（KB/s）
- `YUAN_ADMIN_TOKEN`：设置后需要 `Authorization: Bearer <token>` 调用后台 API

## 后台 API

- `GET /admin/api/overview`
- `POST /admin/api/bt/control`，body: `{"action":"start|stop"}`
- `POST /admin/api/bt/torrent`，body: `{"torrent_path":"...","save_path":"..."}`
- `POST /admin/api/bt/settings`，body: `{"max_peers":80,"listen_port":6881,"download_limit_kbps":1024,"upload_limit_kbps":256}`
- `GET /admin/api/bt/history?page=1&page_size=20`

## 说明

- `download_limit_kbps/upload_limit_kbps` 已接入 BT core 令牌桶限速（单位 KB/s，`0` 表示不限速）。
- Web 后台支持导出/导入配置模板：
  - 导出：根据当前运行态生成 `bt_downloader.config.template.json`
  - 导入：先应用 `bt` 设置，再按 `torrent_file/save_path` 自动加载任务（若提供）
