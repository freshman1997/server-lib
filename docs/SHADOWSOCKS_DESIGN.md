# Shadowsocks 协议接入设计与实施清单（MVP -> 可用）

## 1. 目标与结论

本文档给出在当前 `server-lib` 架构下接入 Shadowsocks 的最小可用实现方案（MVP），并明确后续可扩展路径。

结论：当前项目具备实现条件，建议先落地 `AEAD 2018` 兼容的服务端能力（TCP + UDP），再按需扩展到本地转发器、插件链路与更高版本协议。

## 2. 设计范围

### 2.1 本期范围（MVP）

- 新增 `protocol/shadowsocks/` 协议模块
- 支持 Shadowsocks 服务端监听（单端口 TCP + UDP）
- 支持 AEAD 方法：
  - `aes-128-gcm`
  - `aes-256-gcm`
  - `chacha20-ietf-poly1305`
- 支持目标地址类型：IPv4 / IPv6 / Domain
- 支持 TCP 中继与 UDP relay
- 复用现有 ACL / SSRF 防护 / 超时 / 连接上限策略
- 新增协议级测试与互通测试（与常见 SS 客户端）

### 2.2 暂不纳入（后续）

- 旧版 stream cipher（如 `aes-256-cfb`）
- 2022 系列方法（`2022-blake3-*`）
- obfs/v2ray-plugin 等插件协议
- 多用户动态密钥管理与在线热更新

## 3. 参考规范

- Shadowsocks AEAD 规范（community SIP 文档）
- SOCKS5 RFC 1928（仅用于本项目内部桥接场景参考）
- 项目现有加密与网络抽象实现（OpenSSL EVP + Async runtime）

说明：Shadowsocks 非 IETF RFC 标准协议，工程上以主流客户端互通行为为准。

## 4. 现有能力映射

### 4.1 可直接复用

- 网络运行时与异步连接：`core/core/include/net/runtime/network_runtime.h`
- 监听与连接上下文：`core/core/include/net/async/async_listener_host.h`
- SOCKS5 的会话与转发模型：`protocol/socks5/src/socks5_server.cpp`
- OpenSSL EVP 封装与 AEAD 能力：`protocol/ssh/src/crypto/ssh_crypto_openssl.cpp`
- 代理安全治理（ACL / SSRF / 限额）可直接借鉴：`server/proxy/include/forward_proxy_service.h`

### 4.2 必须新增

- Shadowsocks 报文编解码（TCP 分块、UDP 单包）
- Shadowsocks 密钥派生与 nonce 管理
- Shadowsocks 专用配置结构与服务封装

## 5. 架构方案

### 5.1 分层

```
Application / Service
    -> ShadowsocksService
        -> ShadowsocksServer
            -> ShadowsocksSession (TCP)
            -> UdpAssociation (UDP)
                -> Core Runtime (Connection/Datagram/EventLoop)
                    -> OpenSSL EVP (AEAD)
```

### 5.2 目录建议

```
protocol/shadowsocks/
  CMakeLists.txt
  include/
    shadowsocks_server.h
    shadowsocks_config.h
    shadowsocks_protocol.h
    shadowsocks_packet_codec.h
    shadowsocks_crypto.h
  src/
    shadowsocks_server.cpp
    shadowsocks_session.cpp
    shadowsocks_packet_codec.cpp
    shadowsocks_crypto_openssl.cpp
```

测试目录：

```
test/protocol/shadowsocks/
  test_shadowsocks_codec.cpp
  test_shadowsocks_crypto.cpp
  test_shadowsocks_tcp_relay.cpp
  test_shadowsocks_udp_relay.cpp
```

## 6. 配置模型

```cpp
struct ShadowsocksServerConfig {
    std::string listen_host = "0.0.0.0";
    int port = 8388;

    std::string method = "chacha20-ietf-poly1305";
    std::string password;

    bool enable_tcp = true;
    bool enable_udp = true;

    uint32_t connect_timeout_ms = 10000;
    uint32_t idle_timeout_ms = 300000;
    uint32_t udp_idle_timeout_ms = 300000;
    size_t max_connections = 8192;
    size_t max_sessions_per_client = 0;
    size_t max_datagram_size = 65535;

    bool allow_private_targets = false;
    std::vector<std::string> allow_targets;
    std::vector<std::string> deny_targets;
};
```

补充约束：

- `password` 不能为空
- `method` 必须在受支持列表中
- 端口范围 `1-65535`

## 7. 协议处理要点

### 7.1 TCP（AEAD）

会话阶段：

1. 读取并缓存 `salt`
2. 基于 `password + method + salt` 派生会话子密钥
3. 按 AEAD 分块解密：先长度块，再数据块
4. 首个明文 payload 解析 `ATYP + DST.ADDR + DST.PORT`
5. 建立上游连接并进入双向 relay

发送方向：

- 上游回包按 AEAD 分块加密后回写客户端
- 每块递增 nonce（发送与接收方向各自独立计数）

### 7.2 UDP（AEAD）

每个 datagram 独立处理：

1. 解析 `salt + ciphertext + tag`
2. 解密得到 `ATYP + DST.ADDR + DST.PORT + DATA`
3. 转发到目标
4. 返回包按同方法重新封装加密发回客户端

说明：UDP 为无连接模型，需做客户端地址绑定与空闲回收。

## 8. 密码学设计

### 8.1 抽象接口

新增 `ShadowsocksCrypto` 接口，职责：

- method 元信息（key/salt/nonce/tag 长度）
- master key 派生
- subkey 派生
- AEAD encrypt/decrypt
- nonce 自增工具

### 8.2 OpenSSL 实现

使用 EVP：

- AES-GCM: `EVP_aes_128_gcm` / `EVP_aes_256_gcm`
- ChaCha20-Poly1305: `EVP_chacha20_poly1305`

建议：

- 统一使用 `uint8_t` 缓冲区
- 加解密失败返回明确错误码并计数
- 关键材料在生命周期末清零（best effort）

## 9. 与现有模块集成

### 9.1 CMake 改动

- 根 `CMakeLists.txt` 增加 `add_subdirectory(protocol/shadowsocks)`
- `test/protocol/CMakeLists.txt` 增加 `shadowsocks` 子目录
- 如需服务化运行，新增 `server/services` 的 `ShadowsocksService`

### 9.2 服务封装

接口风格对齐现有服务：

- `init() / start() / stop()`
- 运行时注入（`RuntimeContextAwareService`）
- 统一事件上报（accepted/rejected/timeout/close reason）

## 10. 测试策略

### 10.1 单元测试

- method 参数检查（key/salt/nonce/tag 长度）
- KDF 与 nonce 递增正确性
- TCP 分块编解码正确性（含粘包/拆包）
- UDP 封包解析与重组

### 10.2 集成测试

- TCP 透传：HTTP/HTTPS 请求通过 SS 客户端打通
- UDP 透传：DNS 查询 / 本地 UDP Echo
- 异常路径：错误密码、篡改 tag、超长包、非法 ATYP

### 10.3 互通测试

建议至少覆盖：

- `shadowsocks-rust` 客户端
- `shadowsocks-libev` 客户端
- Windows 常见 GUI 客户端（使用相同 method/password）

验收标准：TCP/UDP 均能稳定收发，错误密钥必失败，日志可定位失败原因。

## 11. 分阶段落地计划

### Phase 1（已完成）

- 搭建模块骨架 + 配置 + CMake
- 实现 crypto/method 元信息
- 完成基础地址编解码与首批单测

### Phase 2（进行中）

- 完整 TCP relay（并发、超时、关闭语义）
- UDP relay 与 idle 回收
- ACL / SSRF / 限流策略接入

### Phase 3（待开始）

- 单元测试 + 集成测试 + 互通验证
- 文档与运维参数整理

## 12. 风险与规避

- 分块/nonce 细节错误导致互通失败
  - 规避：先写 codec/crypto 金丝雀测试，再接网络链路
- UDP 地址绑定与 NAT 场景复杂
  - 规避：先实现单客户端稳定模式，再扩展多客户端并发
- method 兼容差异
  - 规避：MVP 只做 3 个主流 AEAD 方法，优先互通覆盖

## 13. 里程碑验收

- `protocol/shadowsocks` 可独立编译链接
- 本地端到端：SS 客户端可经本服务访问 `https://example.com`
- UDP DNS 请求可通
- 测试目录新增并纳入 CI（如当前仓库开启测试构建）

## 14. 当前实现状态（代码对齐）

截至当前版本，以下能力已在仓库落地：

- 模块与构建接入：
  - `protocol/shadowsocks/CMakeLists.txt`
  - 根 `CMakeLists.txt` 已接入 `add_subdirectory(protocol/shadowsocks)`
  - `test/protocol/CMakeLists.txt` 已接入 `add_subdirectory(shadowsocks)`
- 基础头文件与结构：
  - `protocol/shadowsocks/include/shadowsocks_protocol.h`
  - `protocol/shadowsocks/include/shadowsocks_config.h`
  - `protocol/shadowsocks/include/shadowsocks_packet_codec.h`
  - `protocol/shadowsocks/include/shadowsocks_crypto.h`
  - `protocol/shadowsocks/include/shadowsocks_server.h`
  - `protocol/shadowsocks/include/shadowsocks_session.h`
- 已实现核心基础能力：
  - method 解析与参数规格：`src/shadowsocks_protocol.cpp`
  - master key 派生与 nonce 递增：`src/shadowsocks_crypto.cpp`
  - 目标地址编解码（IPv4/IPv6/Domain）：`src/shadowsocks_packet_codec.cpp`
  - server/session 运行骨架：`src/shadowsocks_server.cpp`、`src/shadowsocks_session.cpp`
- 测试现状：
  - `test/protocol/shadowsocks/test_shadowsocks.cpp` 已覆盖 method/spec、nonce、key 派生、地址编解码
  - 当前测试目标可构建并通过

## 15. 完整协议支持缺口清单（必须补齐）

以下条目为“具备互通可用性”前的必做项。

### 15.1 P0 协议正确性

- 完整 AEAD 流程：
  - subkey 派生（基于 `master_key + salt`）
  - AEAD encrypt/decrypt（含 tag 校验）
  - method 路由（AES-GCM / ChaCha20-Poly1305）
- TCP 分块协议：
  - 按 Shadowsocks AEAD chunk 处理 `len chunk + payload chunk`
  - 粘包/拆包状态机（可重入、可续读）
  - 首包地址与后续 payload 的阶段切换
- UDP AEAD 报文：
  - 单包解密并解析目标地址
  - 单包加密返回
  - 客户端会话映射与空闲回收

### 15.2 P0 兼容性细节

- nonce 双方向独立（client->server / server->client）
- 加解密失败与 tag 失败统一错误处理
- method 约束校验与错误码对齐
- 与主流客户端互通验证（libev / rust / Windows GUI）

### 15.3 P1 工程化能力

- 配置校验收敛：`password/method/port/max_*`
- 安全策略接入：ACL / SSRF / 私网目标限制
- 可观测性：会话计数、解密失败、tag 失败、udp 丢弃、超时统计
- 优雅关闭：drain 机制与连接回收

### 15.4 P1 测试补齐

- crypto 向量测试（固定 key/salt/nonce/aad）
- TCP chunk 编解码单测（半包、乱序输入、坏 tag）
- UDP 封解包单测
- 端到端 relay 测试（TCP/UDP 各一）
- 错误密码与篡改包失败测试

## 16. 后续推进建议（执行顺序）

建议严格按以下顺序推进，降低返工风险：

1. **先补密码学闭环**：完善 `ShadowsocksCrypto`（subkey + AEAD + nonce 约束）
2. **再补协议编解码闭环**：实现 TCP chunk codec 与 UDP AEAD codec
3. **接入 server 会话状态机**：先 TCP relay，再 UDP relay
4. **接入安全与治理能力**：ACL/SSRF/并发与超时限制
5. **完成互通矩阵**：至少覆盖 rust/libev/Windows 客户端

## 17. DoD（Definition of Done）

以下全部满足，才视为“Shadowsocks 协议支持完整可用”：

- TCP/UDP relay 在默认配置下可稳定工作
- `aes-128-gcm` / `aes-256-gcm` / `chacha20-ietf-poly1305` 均互通通过
- 错误密码、坏 tag、非法包均可稳定拒绝并记录原因
- 压测下无明显内存泄漏与连接泄漏
- 测试集覆盖单测 + 集成 + 互通，并可在 CI 复现

---

建议下一步优先进入「15.1 P0 协议正确性」第 1 条：完成 `ShadowsocksCrypto` 的 subkey 与 AEAD 接口，然后再推进 TCP/UDP codec 与 server 状态机。
