# FileSync Release Tool

一个不依赖 rsync 的轻量文件同步工具，包含服务端和客户端两个入口。

- `release_filesync_server`：推荐运行在 Linux。
- `release_filesync_client`：推荐运行在 Windows。

两个入口使用同一套 peer-to-peer 同步逻辑：每一端都会监听端口、周期扫描本地路径，并主动连接对端交换清单。同步语义是“并集合并”，不是镜像删除：任意一端新增或更新文件，另一端缺失或版本更旧时会拉取该文件；某端独有的文件不会因为对端没有而被删除。

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
  "scan_interval_ms": 1000,
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
  "peer_host": "192.168.1.20",
  "peer_port": 9096,
  "token": "change-me",
  "conflict_strategy": "keep_both",
  "scan_interval_ms": 1000,
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

`conflict_strategy` 当前支持：

- `keep_both`：默认策略。只要本地已有同名但内容不同的文件，就保留本地原文件，把远端版本保存为 `原文件名.conflict.YYYYMMDDHHMMSS`。
- `newer_wins`：较新的远端文件会直接覆盖本地旧文件。

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

## 协议说明

工具使用项目里的 `Core` 网络层（`NetworkRuntime`、`StreamServerSession`、`StreamClientSession`）承载一个自定义同步协议：

- `HELLO filesync/2 <token>`
- `MANIFEST`：发送本端文件清单
- `NEED`：对端返回缺失或更旧的路径
- `FILES`：发送对端需要的目录和文件

文件完整性用 FNV-1a 64 位哈希校验。路径会做相对路径校验，避免写出配置路径。
