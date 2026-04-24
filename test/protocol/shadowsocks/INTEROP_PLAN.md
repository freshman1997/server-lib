# Shadowsocks 互通与集成测试计划

## 目标

- 验证当前 `protocol/shadowsocks` 与主流客户端互通
- 验证 TCP/UDP 实际转发链路（不仅是协议单测）
- 为后续 CI 引入可执行的集成测试脚本

## 当前基线

- 已通过协议单测：method/spec、subkey、AEAD、坏 tag、TCP chunk
- 当前测试入口：`test/protocol/shadowsocks/test_shadowsocks.cpp`

## 手工互通矩阵（建议先执行）

1. `shadowsocks-rust` 客户端
2. `shadowsocks-libev` 客户端
3. Windows GUI 客户端（同 method/password）

每个客户端至少覆盖：

- method: `chacha20-ietf-poly1305`
- method: `aes-128-gcm`
- method: `aes-256-gcm`
- TCP: HTTP/HTTPS 请求
- UDP: DNS 查询

## 标准测试步骤

### 1) 启动服务端

- 服务端监听：`127.0.0.1:8388`
- `password`: `secret`
- `method`: `chacha20-ietf-poly1305`
- 打开 TCP/UDP

### 2) 客户端发起代理

- 配置与服务端一致
- 本地 SOCKS5 监听例如 `127.0.0.1:1080`

### 3) TCP 验证

- `curl --socks5-hostname 127.0.0.1:1080 https://example.com`
- 检查返回状态码与响应内容

### 4) UDP 验证

- 将 DNS 查询走代理（或客户端自带 UDP 测试）
- 验证可解析 `example.com`

### 5) 异常验证

- 错误密码：连接必须失败
- 错误 method：连接必须失败
- 篡改包（客户端若支持）：服务端应拒绝

## 验收标准

- 三种 AEAD method 均可稳定互通
- TCP/UDP 在主流客户端上均可工作
- 错误密码或坏 tag 失败且日志可定位
- 连续运行 5 分钟无崩溃

## 后续自动化建议

- 新增 `test_shadowsocks_integration.cpp`
  - 本地 echo 上游
  - 启动 shadowsocks server
  - 原生客户端模拟器发送加密流量
- 新增 `scripts/shadowsocks/interop_smoke.ps1`
  - 自动拉起 client/server 并执行 curl/dns 检查

## 当前自动化脚本（已提供）

- 启动工具：`test/protocol/shadowsocks/shadowsocks_server_tool.cpp`
- 互通脚本：`test/protocol/shadowsocks/run_shadowsocks_interop.ps1`

示例：

```powershell
pwsh -File .\test\protocol\shadowsocks\run_shadowsocks_interop.ps1 -ClientType rust
```

如客户端不在 PATH：

```powershell
pwsh -File .\test\protocol\shadowsocks\run_shadowsocks_interop.ps1 -ClientType rust -ClientPath "C:\tools\sslocal.exe"
```
