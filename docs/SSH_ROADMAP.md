# SSH 协议实现路线图

## 实现步骤总览

```
Phase 0: 基础设施 ────────────────────────────────────── 约 1 周
Phase 1: Transport Layer ─────────────────────────────── 约 2 周
Phase 2: Authentication Layer ────────────────────────── 约 1 周
Phase 3: Connection Layer ────────────────────────────── 约 1.5 周
Phase 4: SFTP 子系统 ────────────────────────────────── 约 1.5 周
Phase 5: 端口转发 ───────────────────────────────────── 约 1 周
Phase 6: 集成与加固 ─────────────────────────────────── 约 1 周
```

每个 Phase 产出可测试、可运行的增量。Phase 完成后必须通过对应的单元测试和集成测试才能进入下一 Phase。

---

## Phase 0: 基础设施

**目标：** 搭建 SSH 协议的骨架代码、构建系统、协议常量定义、加密原语层。

### 0.1 目录结构与构建系统

- [ ] 创建 `protocol/ssh/` 目录结构
- [ ] 编写 `protocol/ssh/CMakeLists.txt`，定义 `SshProto` 库目标
- [ ] 修改顶层 `CMakeLists.txt`，添加 `add_subdirectory(protocol/ssh)`
- [ ] 修改 `server/services/CMakeLists.txt`，链接 `SshProto`
- [ ] 添加 zlib git submodule 到 `third_party/zlib`
- [ ] 添加 CMake 选项 `YUAN_ENABLE_SSH` 和 `YUAN_ENABLE_SSH_SFTP`

### 0.2 协议常量

- [ ] `ssh_constants.h` — 消息类型枚举、断开原因码、错误码、通道类型常量
- [ ] `ssh_structures.h` — 各消息的结构体定义

### 0.3 消息编解码

- [ ] `ssh_message_codec.h/.cpp` — SSH 消息级编解码
  - mpint 编解码（SSH 多精度整数）
  - string 编解码（uint32 长度前缀 + 数据）
  - name-list 编解码（逗号分隔的算法列表）
  - 各消息类型的 encode/decode 函数

### 0.4 加密原语层

- [ ] `ssh_crypto.h` — 抽象接口
- [ ] `ssh_crypto_openssl.h/.cpp` — OpenSSL 实现
  - SHA-256 / SHA-512
  - HMAC-SHA-256 / HMAC-SHA-512 / HMAC-SHA1
  - AES-CTR-128/192/256 加解密
  - AES-GCM-128/256 加解密
  - ChaCha20-Poly1305 加解密
  - DH 共享密钥计算
  - ECDH 共享密钥计算（NIST P-256/P-384/P-521）
  - Curve25519 共享密钥计算
  - RSA/ECDSA/Ed25519 签名与验证
  - 随机数生成

- [ ] `ssh_key_derivation.h/.cpp` — SSH 密钥派生
  - `derive_key(K, H, session_id, letter, key_len)` — RFC 4253 Section 7.2
  - `derive_exchange_hash(...)` — 各 KEX 算法的 hash 计算

### 0.5 算法策略接口

- [ ] `ssh_kex_algorithm.h` — KEX 算法接口
- [ ] `ssh_host_key_algorithm.h` — 主机密钥算法接口
- [ ] `ssh_cipher.h` — 加密算法接口
- [ ] `ssh_mac.h` — MAC 算法接口
- [ ] `ssh_compression.h` — 压缩算法接口

### 0.6 算法注册表

- [ ] `ssh_algorithm_registry.h/.cpp` — 注册与管理所有算法
  - 各类算法的注册接口
  - 算法协商函数 `negotiate(our_prefs, peer_prefs)`
  - 工厂方法 `create_kex(name)` 等

### 0.7 验收标准

- [ ] `SshProto` 库可成功编译
- [ ] SshCrypto 单元测试通过（加解密正确性、密钥派生正确性）
- [ ] SshAlgorithmRegistry 单元测试通过（注册、协商）
- [ ] SshMessageCodec 单元测试通过（mpint/string/name-list 编解码）

---

## Phase 1: Transport Layer

**目标：** 实现完整的 SSH 传输层，包括版本交换、KEXINIT 协商、密钥交换、NEWKEYS 切换。此阶段完成后，应能完成 SSH 握手的全部过程（但不认证、不处理上层消息）。

### 1.1 版本交换

- [ ] `ssh_version_exchange.h/.cpp`
  - 发送服务端版本字符串 `SSH-2.0-YuanSSH_1.0\r\n`
  - 解析客户端版本字符串
  - 兼容性处理（版本行前的 banner 行、非标准格式）

### 1.2 Binary Packet 编解码

- [ ] `ssh_packet_codec.h/.cpp`
  - 明文模式：packet_length + padding_len + payload + padding
  - Separate MAC 模式：加密 payload + MAC(seq || encrypted)
  - AEAD 模式：packet_length(明文) + 加密(payload+padding) + Tag
  - 填充计算（对齐到 block_size，最小 4 字节）
  - `try_parse()` 确定报文边界
  - `encode()` / `decode()` 编解码

### 1.3 加密上下文

- [ ] `ssh_cipher_context.h/.cpp`
  - 管理双向 cipher / MAC / compression 实例
  - `activate(keys)` 从 NEWKEYS 派生的密钥初始化各算法实例
  - `encrypt(seq, payload)` 加密+MAC
  - `decrypt(seq, packet_buf)` 解密+验证MAC
  - `is_active()` 加密是否已激活

### 1.4 传输层状态机

- [ ] `ssh_transport.h/.cpp`
  - 状态：version_exchanged → kex_init → kex_in_progress → newkeys → active
  - `process_version_exchange()`
  - `process_kex_init(buf)` — 解析对端 KEXINIT，执行算法协商
  - `process_kex_message(msg_type, buf)` — 分发 KEX 交换消息到具体算法
  - `process_newkeys()` — 激活加密上下文
  - `send_kex_init()` — 构造并发送 KEXINIT
  - `send_newkeys()` — 发送 NEWKEYS
  - Rekey 支持（active 状态下收到 KEXINIT 触发）

### 1.5 具体算法实现

- [ ] `kex/ssh_kex_curve25519.cpp` — curve25519-sha256 / curve25519-sha256@libssh.org
- [ ] `kex/ssh_kex_ecdh_nistp.cpp` — ecdh-sha2-nistp256/384/521
- [ ] `kex/ssh_kex_dh_group.cpp` — diffie-hellman-group14-sha256 / group16-sha512 / group18-sha512
- [ ] `hostkey/ssh_hk_ed25519.cpp` — ssh-ed25519
- [ ] `hostkey/ssh_hk_ecdsa.cpp` — ecdsa-sha2-nistp256/384/521
- [ ] `hostkey/ssh_hk_rsa.cpp` — rsa-sha2-512 / rsa-sha2-256 / ssh-rsa
- [ ] `cipher/ssh_cipher_chacha20_poly1305.cpp` — chacha20-poly1305@openssh.com
- [ ] `cipher/ssh_cipher_aes_gcm.cpp` — aes128-gcm@openssh.com / aes256-gcm@openssh.com
- [ ] `cipher/ssh_cipher_aes_ctr.cpp` — aes128-ctr / aes192-ctr / aes256-ctr
- [ ] `mac/ssh_mac_hmac_sha2.cpp` — hmac-sha2-256 / hmac-sha2-512
- [ ] `mac/ssh_mac_hmac_sha1.cpp` — hmac-sha1
- [ ] `compression/ssh_compression_none.cpp` — none
- [ ] `compression/ssh_compression_zlib.cpp` — zlib / zlib@openssh.com

### 1.6 主机密钥管理

- [ ] `ssh_host_key_provider.h/.cpp`
  - 从文件加载主机密钥（OpenSSH 格式）
  - 生成主机密钥对
  - 根据 host_key 算法名查找对应密钥

### 1.7 验收标准

- [ ] SshPacketCodec 单元测试通过（明文/加密/AEAD 模式编解码）
- [ ] SshTransport 单元测试通过（状态机转换）
- [ ] 各 KEX 算法单元测试通过（共享密钥计算正确性）
- [ ] 各 Cipher 单元测试通过（加解密正确性、AEAD tag 验证）
- [ ] **集成测试：** 使用 OpenSSH 客户端连接，完成到 NEWKEYS 的握手（预期断开，因为未实现认证）
- [ ] `ssh -vvv -o "KexAlgorithms=curve25519-sha256" -p 2222 user@localhost` 可看到成功的 KEX

---

## Phase 2: Authentication Layer

**目标：** 实现 SSH 认证层，支持 password、publickey、keyboard-interactive 三种认证方式。此阶段完成后，OpenSSH 客户端可成功认证。

### 2.1 认证状态机

- [ ] `ssh_authenticator.h/.cpp`
  - 状态：auth_start → authenticating → auth_success
  - 处理 SERVICE_REQUEST(ssh-userauth)
  - 处理 USERAUTH_REQUEST
  - 失败计数与最大尝试次数
  - partial_success 多步认证
  - 生成 USERAUTH_FAILURE 的 allowed-methods 列表

### 2.2 认证方法实现

- [ ] `ssh_auth_method.h` — 认证方法接口
- [ ] `ssh_auth_password.cpp` — 密码认证
- [ ] `ssh_auth_publickey.cpp` — 公钥认证
  - 验证签名格式
  - 验证签名内容（session_id + 认证请求数据）
  - 支持 authorized_keys 风格的公钥管理
- [ ] `ssh_auth_keyboard_interactive.cpp` — 键盘交互认证
  - 多轮 challenge/response
  - 通过 Handler 回调构造 challenge

### 2.3 会话对象

- [ ] `ssh_session.h/.cpp`
  - 整合 Transport + Authenticator + ConnectionManager
  - 状态机：connected → version_exchanged → ... → auth_success → active → disconnected
  - 消息分发逻辑
  - 发送/接收报文的高层接口

### 2.4 会话管理器

- [ ] `ssh_session_manager.h/.cpp`
  - 创建/删除/查找会话
  - 会话数量限制

### 2.5 Handler 认证回调

- [ ] `ssh_handler.h` — SshHandler 接口定义（认证相关部分）
  - `on_authenticate(session, username, method, credentials) -> AuthResult`

### 2.6 验收标准

- [ ] SshAuthenticator 单元测试通过（状态机、失败计数、partial_success）
- [ ] 密码认证单元测试通过
- [ ] 公钥认证单元测试通过（签名验证正确性）
- [ ] **集成测试：** OpenSSH 客户端密码认证成功
- [ ] **集成测试：** OpenSSH 客户端公钥认证成功
- [ ] `ssh -o PreferredAuthentications=password user@localhost` 成功认证
- [ ] `ssh -i ~/.ssh/id_ed25519 user@localhost` 成功认证
- [ ] 认证失败超过 max_auth_attempts 后连接断开

---

## Phase 3: Connection Layer

**目标：** 实现 SSH 连接层，支持 session channel、exec 命令、通道流控。此阶段完成后，可通过 `ssh user@host "command"` 执行远程命令。

### 3.1 通道对象

- [ ] `ssh_channel.h/.cpp`
  - 状态机：closed → opening → open → eof → closed
  - 本地/远程窗口大小管理
  - 本地/远程最大包大小
  - channel ID 管理（local_id / remote_id）
  - 数据缓冲（窗口为 0 时缓冲待发送数据）
  - `on_data()` / `on_eof()` / `on_close()` / `on_window_adjust()`

### 3.2 连接层管理

- [ ] `ssh_connection_manager.h/.cpp`
  - 处理 CHANNEL_OPEN 请求
  - 处理 CHANNEL_DATA / CHANNEL_EOF / CHANNEL_CLOSE
  - 处理 CHANNEL_WINDOW_ADJUST
  - 处理 CHANNEL_REQUEST（env, exec, shell, pty, subsystem, signal, window-change）
  - 处理 CHANNEL_OPEN_CONFIRMATION / CHANNEL_OPEN_FAILURE
  - 通道 ID 分配
  - 通道数量限制

### 3.3 全局请求处理

- [ ] `ssh_global_request.h/.cpp`
  - tcpip-forward / cancel-tcpip-forward（本期仅占位，Phase 5 实现）
  - 扩展点注册机制

### 3.4 Channel Handler 回调

- [ ] `ssh_channel_handler.h` — SshChannelHandler 接口
  - `on_open` / `on_data` / `on_eof` / `on_close`
  - `on_request(channel, type, data) -> bool`

### 3.5 Handler 连接层回调

- [ ] 完善 `ssh_handler.h`
  - `on_channel_open(session, type, channel) -> bool`
  - `on_channel_close(session, channel)`
  - `on_channel_data(session, channel, data)`
  - `on_exec_request(session, channel, command) -> bool`
  - `on_subsystem_request(session, channel, name) -> bool`
  - `on_pty_request(session, channel, ...) -> bool` （预留，返回 false）
  - `on_shell_request(session, channel) -> bool` （预留，返回 false）
  - `on_env_request(session, channel, name, value) -> bool`
  - `on_signal(session, channel, signal)`
  - `on_window_change(session, channel, ...)`

### 3.6 子系统注册表

- [ ] `ssh_subsystem_registry.h/.cpp`
  - 注册子系统名 → 工厂函数
  - 创建子系统实例（返回 SshChannelHandler*）

### 3.7 SshServer

- [ ] `ssh_server.h/.cpp`
  - 整合所有组件
  - AsyncListenerHost + coroutine handle_connection
  - 配置管理
  - Handler 设置
  - 子系统注册

### 3.8 Service 包装

- [ ] `server/services/include/ssh_service.h`
- [ ] `server/services/src/ssh_service.cpp`
  - SshService : Service, RuntimeContextAwareService

### 3.9 验收标准

- [ ] SshChannel 单元测试通过（状态机、窗口流控）
- [ ] SshConnectionManager 单元测试通过（通道打开/关闭/数据/EOF）
- [ ] **集成测试：** `ssh user@localhost "echo hello"` 输出 hello
- [ ] **集成测试：** `ssh user@localhost "exit 1"` 退出码为 1
- [ ] **集成测试：** 多个 channel 同时工作
- [ ] **集成测试：** 窗口流控（发送超过窗口大小的数据）
- [ ] PTY/Shell 请求返回失败但不崩溃

---

## Phase 4: SFTP 子系统

**目标：** 实现 SFTP v3 子系统，支持完整的文件操作。此阶段完成后，可通过 `sftp` 命令进行文件传输。

### 4.1 SFTP 报文编解码

- [ ] `ssh_sftp_codec.h/.cpp`
  - SFTP 报文格式：length + type + request_id + payload
  - 各 SFTP 操作的请求/响应编解码
  - SFTP 属性（attrs）编解码

### 4.2 文件系统抽象

- [ ] `ssh_file_system.h` — SshFileSystem 接口
- [ ] `ssh_file_system.cpp` — 基于 POSIX API 的本地文件系统实现
  - open / close / read / write / lstat / fstat / opendir / readdir
  - remove / mkdir / rmdir / realpath / rename / readlink / symlink

### 4.3 SFTP 子系统

- [ ] `ssh_sftp_subsystem.h/.cpp`
  - 实现 SshChannelHandler 接口
  - 处理所有 SFTP v3 操作
  - 文件句柄管理
  - 目录句柄管理
  - 权限检查（通过 Handler 回调）

### 4.4 SFTP 操作列表

| 操作 | SFTP Type | 说明 |
|------|-----------|------|
| SSH_FXP_INIT | 1 | 版本协商 |
| SSH_FXP_VERSION | 2 | 版本响应 |
| SSH_FXP_OPEN | 3 | 打开文件 |
| SSH_FXP_CLOSE | 4 | 关闭文件 |
| SSH_FXP_READ | 5 | 读取文件 |
| SSH_FXP_WRITE | 6 | 写入文件 |
| SSH_FXP_LSTAT | 7 | 获取文件属性（不跟随链接） |
| SSH_FXP_FSTAT | 8 | 获取打开文件属性 |
| SSH_FXP_SETSTAT | 9 | 设置文件属性 |
| SSH_FXP_FSETSTAT | 10 | 设置打开文件属性 |
| SSH_FXP_OPENDIR | 11 | 打开目录 |
| SSH_FXP_READDIR | 12 | 读取目录 |
| SSH_FXP_REMOVE | 13 | 删除文件 |
| SSH_FXP_MKDIR | 14 | 创建目录 |
| SSH_FXP_RMDIR | 15 | 删除目录 |
| SSH_FXP_REALPATH | 16 | 规范化路径 |
| SSH_FXP_STAT | 17 | 获取文件属性（跟随链接） |
| SSH_FXP_RENAME | 18 | 重命名 |
| SSH_FXP_READLINK | 19 | 读取符号链接 |
| SSH_FXP_SYMLINK | 20 | 创建符号链接 |

### 4.5 验收标准

- [ ] SshSftpCodec 单元测试通过
- [ ] SshFileSystem 单元测试通过（基于临时目录的文件操作）
- [ ] **集成测试：** `sftp user@localhost` 连接成功
- [ ] **集成测试：** `sftp` put/get/ls/cd/mkdir/rmdir/remove/rename
- [ ] **集成测试：** 大文件传输（>1GB）
- [ ] **集成测试：** 符号链接操作

---

## Phase 5: 端口转发

**目标：** 实现本地端口转发和远程端口转发。此阶段完成后，SSH 服务具备完整的代理能力。

### 5.1 端口转发管理

- [ ] `ssh_port_forwarding.h/.cpp`
  - direct-tcpip 通道处理（本地转发）
  - forwarded-tcpip 通道处理（远程转发）
  - tcpip-forward 全局请求处理（监听端口）
  - cancel-tcpip-forward 全局请求处理（取消监听）
  - relay_pipe 协程（复用 SOCKS5 模式）

### 5.2 Handler 转发回调

- [ ] 完善 `ssh_handler.h`
  - `on_tcpip_forward(session, bind_addr, bind_port) -> uint16_t`
  - `on_cancel_tcpip_forward(session, bind_addr, bind_port)`
  - `on_direct_tcpip(session, channel, target_host, target_port) -> bool`

### 5.3 验收标准

- [ ] SshPortForwarding 单元测试通过
- [ ] **集成测试：** 本地端口转发 `ssh -L 8080:target:80 user@ssh_server` + `curl http://localhost:8080`
- [ ] **集成测试：** 远程端口转发 `ssh -R 9090:localhost:90 user@ssh_server`
- [ ] **集成测试：** 转发高吞吐数据（iperf3 测试）
- [ ] **集成测试：** 转发期间 rekey 不中断

---

## Phase 6: 集成与加固

**目标：** 全面集成、错误处理完善、安全加固、性能优化。

### 6.1 Rekey 完善

- [ ] 主动 rekey（数据量阈值/时间阈值触发）
- [ ] Rekey 期间数据报文正确排队
- [ ] Rekey 失败处理

### 6.2 错误处理与安全

- [ ] 所有非法报文发送 DISCONNECT
- [ ] 防止 SSH 协议降级攻击
- [ ] 认证超时保护
- [ ] 空闲连接超时
- [ ] 最大报文大小限制
- [ ] 防止窗口耗尽攻击
- [ ] 日志审计（认证成功/失败、通道操作、端口转发）

### 6.3 延迟压缩

- [ ] zlib@openssh.com 延迟压缩（认证完成后才开始压缩）

### 6.4 SSH 扩展协商 (RFC 8308)

- [ ] SSH_MSG_EXT_INFO 支持
- [ ] 服务器通告支持的扩展

### 6.5 全面测试

- [ ] 多种算法组合测试
- [ ] 异常断开恢复测试
- [ ] 并发连接压力测试
- [ ] 内存泄漏检查
- [ ] 与不同 SSH 客户端兼容性测试（OpenSSH, PuTTY, WinSCP, FileZilla）

### 6.6 文档

- [ ] 更新 README.md
- [ ] 更新 PROJECT_OUTLINE.md
- [ ] 示例 main.cpp 集成

### 6.7 验收标准

- [ ] 所有单元测试通过
- [ ] 所有集成测试通过
- [ ] OpenSSH 兼容性测试通过
- [ ] 无内存泄漏
- [ ] 文档更新完成

---

## 依赖关系图

```
Phase 0 (基础设施)
  │
  ├── Phase 1 (Transport Layer)
  │     │
  │     ├── Phase 2 (Authentication Layer)
  │     │     │
  │     │     └── Phase 3 (Connection Layer)
  │     │           │
  │     │           ├── Phase 4 (SFTP)
  │     │           │
  │     │           └── Phase 5 (端口转发)
  │     │
  │     └─────────────── Phase 3 可与 Phase 4/5 并行开发
  │
  └── Phase 6 (集成与加固) ←── 依赖所有 Phase 完成
```

---

## 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| OpenSSL curve25519 支持不完整 | KEX 算法受限 | 优先实现 ECDH-NIST 和 DH-Group 作为后备 |
| SSH 报文格式与测试客户端不兼容 | 集成测试失败 | 逐字节对比 OpenSSH 报文，参考 libssh 实现 |
| zlib 延迟压缩状态管理复杂 | 压缩功能延迟 | Phase 6 才实现压缩，none 模式先行 |
| 公钥格式解析（OpenSSH 私钥格式） | 主机密钥加载失败 | 首期仅支持 PEM 格式，后续支持 OpenSSH 新格式 |
| AEAD 模式报文边界处理 | 解密失败 | 单独对 chacha20-poly1305 和 aes-gcm 编写详尽测试 |
| Channel 窗口流控死锁 | 数据传输卡死 | 参考OpenSSH 窗口策略，阈值触发主动调整 |

---

## 参考实现

实现过程中可参考的开源项目：

| 项目 | 语言 | 许可证 | 参考价值 |
|------|------|--------|---------|
| OpenSSH | C | BSD | 算法实现、报文格式、状态机 |
| libssh | C | LGPL | 报文编解码、通道管理 |
| dropbear | C | MIT | 轻量级实现、嵌入式友好 |
| russh | Rust | Apache-2.0 | 算法策略模式、异步架构 |
| ssh2 | Go | BSD | 算法协商、SFTP 实现 |
