# FileSync Release Tool

一个不依赖 rsync 的轻量文件同步工具，包含服务端和客户端两个入口。

- `release_filesync_server`：推荐运行在 Linux。
- `release_filesync_client`：推荐运行在 Windows。

两个入口使用同一套 peer-to-peer 同步逻辑：每一端都会监听端口、周期扫描本地路径，并主动连接对端交换清单。默认同步语义是“并集合并”：任意一端新增或更新文件，另一端缺失或版本更旧时会拉取该文件；某端独有的文件不会因为对端没有而被删除。需要同步删除时可开启 `sync_deletes`。

## 配置

客户端配置 `client_config.json`：

```json
{
  "listen_host": "0.0.0.0",
  "listen_port": 9096,
  "peer_host": "192.168.1.10",
  "peer_port": 9095,
  "token": "change-me",
  "conflict_strategy": "keep_both",
  "sync_deletes": false,
  "scan_interval_ms": 1000,
  "chunk_size": 32768,
  "include_extensions": [],
  "include_patterns": [],
  "exclude_patterns": [
    "**/.git/**",
    "**/node_modules/**",
    "*.tmp",
    "*.log"
  ],
  "paths": [
    {
      "local": "C:/work/to-sync",
      "remote_prefix": "work"
    }
  ]
}
```

服务端配置 `server_config.json`：

```json
{
  "listen_host": "0.0.0.0",
  "listen_port": 9095,
  "peer_host": "",
  "peer_port": 9096,
  "token": "change-me",
  "conflict_strategy": "keep_both",
  "sync_deletes": false,
  "scan_interval_ms": 1000,
  "chunk_size": 32768,
  "include_extensions": [],
  "include_patterns": [],
  "exclude_patterns": [
    "**/.git/**",
    "**/node_modules/**",
    "*.tmp",
    "*.log"
  ],
  "paths": [
    {
      "local": "/opt/filesync/data",
      "remote_prefix": "work"
    }
  ]
}
```

`remote_prefix` 用来指定同步空间里的相对前缀。两端配置相同的 `remote_prefix` 时，`C:/work/to-sync/a.txt` 会和 Linux 的 `/opt/filesync/data/a.txt` 对应到同一个同步路径 `work/a.txt`。

内网/NAT 场景建议：让客户端配置 `peer_host` 指向服务端，服务端把 `peer_host` 留空。这样由客户端主动连服务端后，即可在同一连接内完成双向同步（先客户端->服务端，再服务端->客户端），服务端不需要知道客户端 IP。之后客户端按 `scan_interval_ms` 周期触发并定时检查。

`conflict_strategy` 当前支持：

- `keep_both`：默认策略。只要本地已有同名但内容不同的文件，就保留本地原文件，把远端版本保存为 `原文件名.conflict.YYYYMMDDHHMMSS`。
- `newer_wins`：较新的远端文件会直接覆盖本地旧文件。

删除同步：

- `sync_deletes: false`：默认关闭，保持并集合并，不传播删除。
- `sync_deletes: true`：开启删除同步。工具会在同步目录下维护 `.filesync_state.json`，只有“上次同步已存在、本次远端缺失、本地仍存在”的路径才会被删除，避免首次连接时误删某端已有文件。

大文件：

- `chunk_size` 控制文件传输分块大小，默认 `32768` 字节，最大 `32768` 字节。协议使用十六进制文本编码，单个网络包约为分块大小的 2 倍，保持较小分块可以避免触发底层连接的大包限制。
- 文件内容按 `PUT_BEGIN`、多行 `CHUNK`、`PUT_END` 流式发送并落盘，不再把整份文件一次性拼成单个网络包。

过滤配置：

- `include_extensions`：只包含指定扩展名。空数组表示不按扩展名过滤，例如 `[".txt", ".json"]`。
- `include_patterns`：只包含匹配的同步相对路径。空数组表示全部包含，例如 `["work/docs/**", "*.md"]`。
- `exclude_patterns`：排除匹配的同步相对路径，例如 `["**/.git/**", "**/node_modules/**", "*.tmp", "*.log"]`。

过滤匹配使用 glob 风格，匹配对象是同步空间里的相对路径，也就是包含 `remote_prefix` 后的路径，例如 `work/a.txt`。

## 运行

Linux 服务端：

```bash
./release_filesync_server server_config.json
```

Windows 客户端：

```powershell
.\release_filesync_client.exe .\client_config.json
```

本地自验证（Linux）：

```bash
./release/filesync/validate_local.sh
```

脚本会自动验证：双向同步、3MB 大文件同步校验、删除同步（`sync_deletes=true`）以及客户端进程稳定性。

## 协议说明

工具使用项目里的 `Core` 网络层（`NetworkRuntime`、`StreamServerSession`、`StreamClientSession`）承载一个自定义同步协议：

- `HELLO filesync/2 <token>`
- `MANIFEST`：发送本端文件清单
- `NEED`：对端返回缺失或更旧的路径
- `FILES`：发送对端需要的目录和文件
- `DELETE`：开启 `sync_deletes` 时在 `NEED` 中通知对端删除本端已删除的路径
- `PUT_BEGIN` / `CHUNK` / `PUT_END`：分块传输文件内容，支持大文件

文件完整性用 FNV-1a 64 位哈希校验。路径会做相对路径校验，避免写出配置路径。
