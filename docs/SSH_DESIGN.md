# SSH 协议实现设计文档

## 1. 概述

本文档描述基于 `server-lib` 架构新增 SSH（Secure Shell）协议的完整实现设计。SSH 是最广泛使用的远程管理和文件传输安全协议，本实现覆盖 SSH-2.0 协议族（RFC 4250-4256），支持完整的传输层、认证层、连接层协议，以及 SFTP 子系统和端口转发功能。

### 1.1 参考规范

| 规范 | 说明 |
|------|------|
| RFC 4250 | SSH Protocol Assigned Numbers |
| RFC 4251 | SSH Protocol Architecture |
| RFC 4252 | SSH Authentication Protocol |
| RFC 4253 | SSH Transport Layer Protocol |
| RFC 4254 | SSH Connection Protocol |
| RFC 4256 | Generic Message Exchange Authentication (keyboard-interactive) |
| RFC 4419 | Diffie-Hellman Group Exchange |
| RFC 5656 | Elliptic Curve Algorithm Integration |
| RFC 6668 | SHA-2 HMAC Algorithms |
| RFC 8268 | More Modular Exponentiation Diffie-Hellman Groups |
| RFC 8270 | Increase Minimum DH Group Size |
| RFC 8308 | Extension Negotiation |
| RFC 8332 | Use of RSA Keys with SHA-256/SHA-512 |
| RFC 8441 | Quota Channel for SSH Agent Forwarding |
| draft-ietf-curdle-ssh-chacha20-poly1305 | chacha20-poly1305@openssh.com |
| draft-ietf-curdle-ssh-ed25519 | ed25519 SSH key type |
| RFC 9272 | FIDO SSH Keys (sk-ssh-ed25519, sk-ecdsa-sha2-nistp256) |

### 1.2 不在范围内（本期）

- PTY 分配与交互式 Shell（预留接口，后续接入）
- SSH Agent 转发
- SSH 证书（用户证书/主机证书）
- SSHFP DNS 记录
- GSSAPI 认证
- SSH-1 协议兼容

### 1.3 后续接入点

以下功能本期不实现，但架构设计预留接入能力：

| 功能 | 预留接口 | 接入方式 |
|------|---------|---------|
| PTY/Shell | `SshChannelHandler::on_pty_request` / `on_shell_request` | 实现 Handler 回调 + fork/exec/ptmx |
| SSH Agent Forwarding | `SshChannelHandler::on_agent_forward` | 预留 channel type + Unix socket 代理 |
| X11 Forwarding | `SshChannelHandler::on_x11_forward` | 预留 channel type + TCP 反向连接 |
| SSH Certificate | `SshHostKeyProvider` / `SshAuthMethod` | 扩展密钥类型和认证方法 |
| SCP | 通过 SFTP 子系统覆盖 | SFTP 实现后 SCP 可通过 exec channel 模拟 |

---

## 2. 设计哲学

### 2.1 层次分明，协议三层严格解耦

SSH 协议天然分层：Transport → Authentication → Connection。我们的实现严格遵循这一分层，每一层拥有独立的状态机、独立的报文处理、独立的配置。层与层之间通过回调/事件通信，不直接共享内部状态。

这与 SMB 的做法不同——SMB 将 session 管理和命令分发耦合在一个 SmbSession 中。SSH 的分层更清晰，因为 Transport 层加密对上层完全透明，Authentication 层认证完成后不再参与，Connection 层独立管理通道。

**原则：协议分层不是代码组织习惯，而是正确性保障。SSH Transport 层的 NEWKEYS 消息切换加密状态，这是一个不可逆的状态跃迁，必须由独立状态机严格控制。**

### 2.2 算法协商是第一公民

SSH 与项目其他协议（HTTP、MQTT、SMB）最本质的区别在于：SSH 的通信参数（加密、MAC、压缩）不是固定的，而是由双方在连接建立时协商决定的。这意味着：

- 不能硬编码算法，必须通过注册表（Registry）模式管理
- 每个算法是独立的策略对象，实现统一接口
- 协商结果决定后续所有 I/O 路径的编解码行为

**原则：算法不是配置项，而是行为策略。协商选出的是策略对象，不是算法名称字符串。**

### 2.3 加密层与 Transport 层正交

SSH 的加密层不是 Transport 的内部实现，而是一个正交的关注点。参考 `SSLHandler` 的集成模式，SSH 的加密层应当：

- 实现 `SshCipher` 统一接口（加解密 + MAC）
- 在 NEWKEYS 之后挂载到 I/O 路径，之前透传
- 与 `Connection` 的读写路径集成，而非在协议层做加解密

这与 SMB 的做法不同——SMB 在 SmbServer 层做加解密。SSH 的报文更频繁，加密是逐包进行的，必须集成到 I/O 路径才能保证性能。

**原则：加密是 I/O 管道的关注点，不是协议报文的关注点。**

### 2.4 通道是资源，不是报文

SSH Connection 层的 channel 不仅是报文类型，更是一种有生命周期的资源。Channel 的 open/data/EOF/close 构成一个完整的资源生命周期。我们的设计将 Channel 视为独立对象，拥有自己的状态机，而非仅仅在 session 中用 map 管理几个字段。

这与 SMB 的 TreeConnect 类似，但 SSH channel 更轻量、数量更多、生命周期更短。Channel 的打开和关闭频率远高于 SMB 的 TreeConnect。

**原则：高频短生命周期资源必须独立建模，不能附属于 session。**

### 2.5 Handler 回调是唯一的扩展点

与项目所有协议一致，业务逻辑通过 Handler 接口注入。SSH 的 Handler 需要覆盖：认证决策、通道打开授权、通道数据处理、端口转发授权。Handler 不是可选的通知接口，而是必须实现的核心决策点。

**原则：协议引擎做协议正确性，Handler 做业务正确性。引擎不决策，Handler 不越权。**

### 2.6 参照而非照搬

SMB 是本项目中复杂度最高的协议实现，是 SSH 实现的最佳参考。但 SSH 与 SMB 有本质区别：

| 维度 | SMB | SSH |
|------|-----|-----|
| 加密模型 | 会话级加密/签名，可选 | 逐包加密，必须 |
| 认证后角色 | 认证完成后继续深度参与 | 认证完成后退出 |
| 通道模型 | TreeConnect（少量、持久） | Channel（大量、短命） |
| 报文格式 | 固定头 + 命令 payload | 二进制包 + 消息类型 |
| 多路复用 | Compound 请求 | Channel 复用单连接 |

**原则：学习 SMB 的架构纪律，但不把 SMB 的设计决策代入 SSH。**

---

## 3. 架构设计

### 3.1 整体分层

```
┌──────────────────────────────────────────────────────────────┐
│                   Application (main.cpp)                      │
│    add_typed_service<SshService>(...)                         │
├──────────────────────────────────────────────────────────────┤
│                    Service Layer                              │
│    SshService : Service                                      │
│              : RuntimeContextAwareService                     │
│    - 持有 SshServer (unique_ptr)                             │
│    - 独立线程运行 NetworkRuntime                              │
│    - 通过 EventBus 发布生命周期事件                            │
├──────────────────────────────────────────────────────────────┤
│                    Protocol Layer                             │
│    ┌─────────────────────────────────────────────┐            │
│    │ SshServer                                    │            │
│    │  - AsyncListenerHost 监听 22                  │            │
│    │  - SshSessionManager 会话管理                  │            │
│    │  - SshAlgorithmRegistry 算法注册               │            │
│    │  - SshHostKeyProvider 主机密钥                 │            │
│    │  - handler_: SshHandler*                     │            │
│    └─────────────────────────────────────────────┘            │
│                                                                │
│    ┌─── Transport Layer ─────────────────────────┐            │
│    │ SshTransport                                 │            │
│    │  - 版本交换                                  │            │
│    │  - 密钥交换 (KEX)                            │            │
│    │  - NEWKEYS 加密切换                          │            │
│    │  - SshPacketCodec 报文编解码                  │            │
│    │  - SshCipherContext 加密上下文                 │            │
│    └──────────────────────────────────────────────┘           │
│                                                                │
│    ┌─── Authentication Layer ─────────────────────┐           │
│    │ SshAuthenticator                             │            │
│    │  - password / publickey / keyboard-interactive│            │
│    │  - 认证方法协商与多步认证                      │            │
│    └──────────────────────────────────────────────┘           │
│                                                                │
│    ┌─── Connection Layer ─────────────────────────┐           │
│    │ SshConnectionManager                         │            │
│    │  - SshChannel 通道管理                       │            │
│    │  - 全局请求处理                              │            │
│    │  - 端口转发 (Local/Remote)                   │            │
│    │  - SshSubsystemRegistry 子系统注册             │            │
│    └──────────────────────────────────────────────┘           │
│                                                                │
│    ┌─── Subsystems ───────────────────────────────┐           │
│    │ SshSftpSubsystem                             │            │
│    │  - SFTP v3-6 文件操作                        │            │
│    │  - SshFileSystem 文件系统抽象                  │            │
│    │  - SshSftpCodec SFTP 报文编解码               │            │
│    └──────────────────────────────────────────────┘           │
│                                                                │
│    ┌─── Crypto ───────────────────────────────────┐           │
│    │ SshCrypto (接口)                             │            │
│    │  - SshCryptoOpenSSL (实现)                    │            │
│    │  - SshKeyDerivation 密钥派生                  │            │
│    │  - SshCipher / SshMac / SshCompression 策略  │            │
│    │  - SshKexAlgorithm 密钥交换算法               │            │
│    │  - SshHostKeyAlgorithm 主机密钥算法            │            │
│    └──────────────────────────────────────────────┘           │
│                                                                │
├──────────────────────────────────────────────────────────────┤
│                    Core Layer                                  │
│    Poller / EventLoop / Acceptor / Connection                  │
│    AsyncListenerHost / AsyncConnectionContext                   │
│    ByteBuffer / Coroutine / TimerManager                       │
└──────────────────────────────────────────────────────────────┘
```

### 3.2 核心类图

```
┌──────────────────────────────────────┐
│         SshServerConfig              │
│  - port (default 22)                 │
│  - host_key_paths                    │
│  - kex_algorithms                    │
│  - cipher_algorithms                 │
│  - mac_algorithms                    │
│  - compression_algorithms            │
│  - auth_methods                      │
│  - max_sessions                      │
│  - max_channels_per_session          │
│  - idle_timeout_ms                   │
│  - max_auth_attempts                 │
│  - banner                            │
│  - enable_port_forwarding            │
│  - enable_sftp                       │
└──────────────────────────────────────┘
          │
          ▼
┌──────────────────────────────────────────────┐
│               SshServer                      │
│──────────────────────────────────────────────│
│  - listener_: AsyncListenerHost              │
│  - session_mgr_: SshSessionManager           │
│  - algo_registry_: SshAlgorithmRegistry      │
│  - host_key_provider_: SshHostKeyProvider    │
│  - subsystem_registry_: SshSubsystemRegistry │
│  - handler_: SshHandler*                     │
│──────────────────────────────────────────────│
│  + init(port) / serve() / stop()             │
│  + set_handler(SshHandler*)                  │
│  + register_subsystem(name, factory)          │
│  - handle_connection(AsyncConnCtx)           │
└──────────────────────────────────────────────┘
          │ 1
          │
          │ *
          ▼
┌──────────────────────────────────────────────┐
│              SshSession                      │
│──────────────────────────────────────────────│
│  - conn_: Connection*                        │
│  - ctx_: AsyncConnectionContext              │
│  - transport_: SshTransport                  │
│  - authenticator_: SshAuthenticator          │
│  - conn_mgr_: SshConnectionManager           │
│  - state_: State                             │
│  - session_id_: vector<uint8_t>              │
│  - username_: string                         │
│  - auth_attempts_: int                       │
│──────────────────────────────────────────────│
│  + state() / username() / session_id()       │
│  + send(msg) / receive()                     │
│  + close()                                   │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│            SshTransport                      │
│──────────────────────────────────────────────│
│  - state_: TransportState                    │
│  - cipher_ctx_: SshCipherContext              │
│  - codec_: SshPacketCodec                    │
│  - kex_algo_: SshKexAlgorithm*               │
│  - host_key_algo_: SshHostKeyAlgorithm*       │
│  - session_id_: vector<uint8_t>              │
│  - send_seq_: uint32_t                       │
│  - recv_seq_: uint32_t                       │
│  - our_kex_init_: vector<uint8_t>            │
│  - peer_kex_init_: vector<uint8_t>           │
│──────────────────────────────────────────────│
│  + process_version_exchange()                │
│  + process_kex_init(buf)                     │
│  + process_kex_message(msg_type, buf)        │
│  + process_newkeys()                         │
│  + encrypt_packet(payload) -> ByteBuffer     │
│  + decrypt_packet(cipher_buf) -> ByteBuffer  │
│  + is_encrypted() const                      │
│  + state() const                             │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│          SshCipherContext                    │
│──────────────────────────────────────────────│
│  - client_cipher_: SshCipher*                │
│  - server_cipher_: SshCipher*                │
│  - client_mac_: SshMac*                      │
│  - server_mac_: SshMac*                      │
│  - client_compressor_: SshCompression*       │
│  - server_compressor_: SshCompression*       │
│  - client_iv_: vector<uint8_t>               │
│  - server_iv_: vector<uint8_t>               │
│  - client_cipher_key_: vector<uint8_t>       │
│  - server_cipher_key_: vector<uint8_t>       │
│  - client_mac_key_: vector<uint8_t>          │
│  - server_mac_key_: vector<uint8_t>          │
│──────────────────────────────────────────────│
│  + activate(keys, session_id)                │
│  + encrypt(seq, payload) -> SshPacket        │
│  + decrypt(seq, packet_buf) -> payload       │
│  + is_active() const                         │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│           SshAuthenticator                   │
│──────────────────────────────────────────────│
│  - state_: AuthState                         │
│  - session_: SshSession*                     │
│  - allowed_methods_: vector<string>          │
│  - auth_attempts_: int                       │
│  - partial_success_: bool                    │
│──────────────────────────────────────────────│
│  + process_userauth_request(msg)             │
│  + process_userauth_info_response(msg)       │
│  + state() const                             │
│  + authenticated() const                     │
│  + username() const                          │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│        SshConnectionManager                  │
│──────────────────────────────────────────────│
│  - channels_: map<uint32, SshChannel>        │
│  - next_channel_id_: uint32_t                │
│  - session_: SshSession*                     │
│  - port_forwarding_: SshPortForwarding       │
│──────────────────────────────────────────────│
│  + process_channel_open(msg)                 │
│  + process_channel_data(channel_id, data)    │
│  + process_channel_close(channel_id)         │
│  + process_global_request(msg)               │
│  + open_channel(type, ...) -> SshChannel&    │
│  + close_channel(channel_id)                 │
│  + send_channel_data(channel_id, data)       │
│  + find_channel(channel_id) -> SshChannel*   │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│            SshChannel                        │
│──────────────────────────────────────────────│
│  - local_id_: uint32_t                       │
│  - remote_id_: uint32_t                      │
│  - type_: string                             │
│  - state_: ChannelState                      │
│  - local_window_: uint32_t                   │
│  - remote_window_: uint32_t                  │
│  - local_max_packet_: uint32_t               │
│  - remote_max_packet_: uint32_t              │
│  - handler_: SshChannelHandler*              │
│  - pending_data_: deque<ByteBuffer>          │
│──────────────────────────────────────────────│
│  + on_open()                                 │
│  + on_data(data)                             │
│  + on_eof()                                  │
│  + on_close()                                │
│  + on_window_adjust(bytes)                   │
│  + on_request(type, data)                    │
│  + write_data(data)                          │
│  + close()                                   │
│  + state() const                             │
│  + type() const                              │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│         SshHandler                           │
│  <<interface>>                               │
│──────────────────────────────────────────────│
│  + on_authenticate(session, username, method,│
│    credentials) -> AuthResult                │
│  + on_channel_open(session, type,            │
│    channel) -> bool                          │
│  + on_channel_close(session, channel)        │
│  + on_channel_data(session, channel, data)   │
│  + on_channel_request(session, channel,      │
│    request_type, request_data) -> bool       │
│  + on_global_request(session, type,          │
│    request_data) -> bool                     │
│  + on_session_opened(session)                │
│  + on_session_closed(session)                │
│  + on_tcpip_forward(session, bind_addr,      │
│    bind_port) -> uint16_t                    │
│  + on_cancel_tcpip_forward(session,          │
│    bind_addr, bind_port)                     │
│  + on_pty_request(session, channel,          │
│    term, w, h, pw, ph, modes) -> bool       │
│  + on_shell_request(session, channel)        │
│     -> bool                                  │
│  + on_exec_request(session, channel,         │
│    command) -> bool                          │
│  + on_subsystem_request(session, channel,    │
│    name) -> bool                             │
│  + on_env_request(session, channel,          │
│    name, value) -> bool                      │
│  + on_window_change(session, channel,        │
│    w, h, pw, ph)                            │
│  + on_signal(session, channel, signal)       │
│  + on_x11_forward(session, channel,          │
│    auth_proto, auth_cookie, screen) -> bool  │
│  + on_agent_forward(session, channel)        │
│     -> bool                                  │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│        SshChannelHandler                     │
│  <<interface>>                               │
│──────────────────────────────────────────────│
│  + on_open(channel)                          │
│  + on_data(channel, data)                    │
│  + on_eof(channel)                           │
│  + on_close(channel)                         │
│  + on_window_adjust(channel, bytes)          │
│  + on_request(channel, type, data) -> bool   │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│      SshAlgorithmRegistry                    │
│──────────────────────────────────────────────│
│  - kex_algos_: map<string, SshKexFactory>    │
│  - host_key_algos_: map<string, HKFactory>   │
│  - cipher_algos_: map<string, CipherFactory> │
│  - mac_algos_: map<string, MacFactory>       │
│  - comp_algos_: map<string, CompFactory>     │
│──────────────────────────────────────────────│
│  + register_kex(name, factory)               │
│  + register_host_key(name, factory)          │
│  + register_cipher(name, factory)            │
│  + register_mac(name, factory)               │
│  + register_compression(name, factory)       │
│  + create_kex(name) -> SshKexAlgorithm*      │
│  + create_host_key(name) -> SshHostKeyAlg*   │
│  + create_cipher(name) -> SshCipher*         │
│  + create_mac(name) -> SshMac*               │
│  + create_compression(name) -> SshComp*      │
│  + supported_kex_names() -> vector<string>   │
│  + supported_host_key_names() -> vector<str> │
│  + supported_cipher_names() -> vector<str>   │
│  + supported_mac_names() -> vector<str>      │
│  + supported_compression_names()-> vector<str>│
│  + negotiate(our_prefs, peer_prefs)           │
│     -> optional<NegotiatedAlgorithms>         │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│        SshCrypto                             │
│  <<interface>>                               │
│──────────────────────────────────────────────│
│  + encrypt(key, iv, data) -> vector<uint8_t> │
│  + decrypt(key, iv, data) -> vector<uint8_t> │
│  + mac(key, seq, data) -> vector<uint8_t>    │
│  + dh_compute_shared(prv, pub)               │
│     -> vector<uint8_t>                       │
│  + ecdh_compute_shared(prv, pub)             │
│     -> vector<uint8_t>                       │
│  + sign(host_key, data) -> vector<uint8_t>   │
│  + verify(host_key, sig, data) -> bool       │
│  + sha256(data) -> vector<uint8_t>           │
│  + sha512(data) -> vector<uint8_t>           │
│  + hmac_sha256(key, data) -> vector<uint8_t> │
│  + hmac_sha512(key, data) -> vector<uint8_t> │
│  + generate_key_pair(type) -> KeyPair        │
│  + derive_ssh_key(K, H, session_id,          │
│    letter, key_len) -> vector<uint8_t>       │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│      SshKeyDerivation                        │
│  <<static utility>>                          │
│──────────────────────────────────────────────│
│  + derive_key(K, H, session_id, letter,      │
│    key_len) -> vector<uint8_t>               │
│  + derive_session_id(K, H) -> vector<uint8_t>│
│  + derive_exchange_hash(K, H, V_C, V_S,     │
│    I_C, I_S, K_S) -> vector<uint8_t>        │
│  + compute_host_key_signature(H, host_key)   │
│     -> vector<uint8_t>                       │
│  + verify_host_key_signature(H, sig, key)    │
│     -> bool                                  │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│     SshPortForwarding                        │
│──────────────────────────────────────────────│
│  - local_forwards_: map<uint32, ForwardEntry>│
│  - remote_forwards_: map<BindKey, Entry>     │
│  - session_: SshSession*                     │
│──────────────────────────────────────────────│
│  + handle_direct_tcpip(channel, host, port)  │
│  + handle_tcpip_forward(bind_addr, bind_port)│
│     -> uint16_t                              │
│  + handle_cancel_tcpip_forward(addr, port)   │
│  + on_forwarded_tcpip_channel(channel)       │
│  - relay_pipe(src, dst) -> Task<void>        │
└──────────────────────────────────────────────┘
```

---

## 4. 协议状态机

### 4.1 会话级状态机

```
  Client                                Server
  ──────                                ──────
  │  TCP Connect (port 22)             │
  │────────────────────────────────────>│  State: connected
  │                                     │
  │  SSH-2.0-<softwareversion>\r\n     │
  │<────────────────────────────────────│  State: version_exchanged
  │                                     │
  │  SSH_MSG_KEXINIT                    │
  │<════════════════════════════════════│  State: kex_init
  │                                     │  → 双方交换算法列表
  │  SSH_MSG_KEXINIT                    │
  │────────────────────────────────────>│
  │                                     │
  │  SSH_MSG_KEXDH_INIT / KEX_ECDH_INIT│
  │────────────────────────────────────>│  State: kex_in_progress
  │                                     │  → 计算共享密钥
  │  SSH_MSG_KEXDH_REPLY /              │
  │  KEX_ECDH_REPLY                     │
  │<────────────────────────────────────│
  │                                     │
  │  SSH_MSG_NEWKEYS                    │
  │<════════════════════════════════════│  State: newkeys
  │                                     │  → 切换到协商的加密算法
  │  SSH_MSG_NEWKEYS                    │
  │────────────────────────────────────>│
  │                                     │
  │  SSH_MSG_SERVICE_REQUEST            │
  │  ("ssh-userauth")                   │
  │────────────────────────────────────>│  State: auth_start
  │  SSH_MSG_SERVICE_ACCEPT             │
  │<────────────────────────────────────│
  │                                     │
  │  SSH_MSG_USERAUTH_REQUEST           │
  │────────────────────────────────────>│  State: authenticating
  │  SSH_MSG_USERAUTH_SUCCESS /         │
  │  SSH_MSG_USERAUTH_FAILURE           │
  │<────────────────────────────────────│
  │                                     │
  │  SSH_MSG_SERVICE_REQUEST            │
  │  ("ssh-connection")                 │
  │────────────────────────────────────>│  State: connection_start
  │  SSH_MSG_SERVICE_ACCEPT             │
  │<────────────────────────────────────│
  │                                     │
  │  SSH_MSG_CHANNEL_OPEN / DATA / ...  │
  │<════════════════════════════════════│  State: active
  │                                     │
  │  SSH_MSG_DISCONNECT                 │
  │────────────────────────────────────>│  State: disconnected
```

### 4.2 状态转换表

| 当前状态 | 事件 | 动作 | 下一状态 |
|---------|------|------|---------|
| connected | 收到版本字符串 | 解析版本，发送服务端版本 | version_exchanged |
| version_exchanged | 收到 KEXINIT | 解析算法列表，执行协商，初始化 KEX | kex_init |
| kex_init | 收到 KEX 交换消息 | 计算 DH/ECDH 共享密钥，生成 exchange hash，派生密钥 | kex_in_progress |
| kex_in_progress | 发送 KEX reply + NEWKEYS | 等待客户端 NEWKEYS | newkeys |
| newkeys | 收到 NEWKEYS | 激活 SshCipherContext，后续报文加解密 | newkeys |
| newkeys | 收到 SERVICE_REQUEST(ssh-userauth) | 发送 SERVICE_ACCEPT | auth_start |
| auth_start | 收到 USERAUTH_REQUEST | 验证凭据，通过 Handler 回调 | authenticating |
| authenticating | 认证成功 | 发送 USERAUTH_SUCCESS | auth_success |
| authenticating | 认证失败 | 发送 USERAUTH_FAILURE，增加 attempts | auth_start / disconnected |
| auth_success | 收到 SERVICE_REQUEST(ssh-connection) | 发送 SERVICE_ACCEPT | connection_start |
| connection_start | 收到 CHANNEL_OPEN 等消息 | 处理通道操作 | active |
| active | 收到任何 Connection 消息 | 分发到对应 channel / 全局请求处理 | active |
| active | 收到 DISCONNECT | 清理所有 channel，关闭连接 | disconnected |
| any | TCP close / EOF | 清理所有资源 | disconnected |
| active | 收到 KEXINIT | 重新密钥交换 | kex_init (rekey) |

### 4.3 Channel 状态机

```
  ┌───────────┐
  │  closed   │ (初始/终止)
  └─────┬─────┘
        │ CHANNEL_OPEN 发送/接收
        ▼
  ┌───────────┐
  │  opening  │
  └─────┬─────┘
        │ CHANNEL_OPEN_CONFIRMATION
        ▼
  ┌───────────┐
  │   open    │ ←── WINDOW_ADJUST
  └──┬────┬───┘
     │    │
     │    │ CHANNEL_EOF
     │    ▼
     │  ┌───────────┐
     │  │    eof     │
     │  └─────┬─────┘
     │        │ CHANNEL_CLOSE
     │        ▼
     │  ┌───────────┐
     │  │   closed   │
     │  └───────────┘
     │
     │ CHANNEL_CLOSE
     ▼
  ┌───────────┐
  │   closed   │
  └───────────┘
```

---

## 5. SSH Transport Layer 设计

### 5.1 SSH Binary Packet 格式

```
未加密时:
┌──────────────────┬─────────────┬─────────────────┬──────────┬──────────┐
│ uint32           │ byte        │ byte[n1]        │ byte[n2] │ byte[m]  │
│ packet_length    │ padding_len │ payload         │ padding  │ MAC      │
│ (= 1+n1+n2)     │             │                 │ (random) │          │
└──────────────────┴─────────────┴─────────────────┴──────────┴──────────┘

加密后 (separate MAC, e.g. aes256-ctr + hmac-sha2-256):
┌──────────────────────────────────────────────────────────┬──────────┐
│ Encrypt( packet_length + padding_len + payload + padding)│ MAC      │
└──────────────────────────────────────────────────────────┴──────────┘

加密后 (AEAD, e.g. chacha20-poly1305, aes128-gcm):
┌──────────────────────────────────────────────────────────────────────┐
│ Encrypt( packet_length + padding_len + payload + padding ) + Tag    │
│ (packet_length 不加密)                                               │
└──────────────────────────────────────────────────────────────────────┘
```

### 5.2 SshPacketCodec

```cpp
class SshPacketCodec {
public:
    // 从 ByteBuffer 尝试解析一个完整 SSH 报文
    // 未加密时: 读取 packet_length 确定边界
    // 加密时: 先读取加密的 packet_length, 解密后确定边界
    struct ParseResult {
        bool complete;
        size_t total_bytes;  // 整个报文占用的字节数
    };
    static ParseResult try_parse(const ByteBuffer& buf, bool encrypted,
                                 SshCipherContext* cipher_ctx,
                                 uint32_t seq);

    // 编码明文 payload 为 SSH 报文
    static ByteBuffer encode(uint32_t seq, const uint8_t* payload, size_t len,
                            SshCipherContext* cipher_ctx);

    // 解码 SSH 报文为明文 payload
    static std::optional<ByteBuffer> decode(uint32_t seq, const uint8_t* data, size_t len,
                                           SshCipherContext* cipher_ctx);

    // 计算填充
    static size_t calculate_padding(size_t payload_len, size_t block_size);
};
```

### 5.3 版本交换

```
客户端: "SSH-2.0-OpenSSH_9.6\r\n"
服务端: "SSH-2.0-YuanSSH_1.0\r\n"
```

- 版本字符串以 `\r\n` 结尾
- 在 `\r\n` 之前的 `"- "` 之后的部分为软件版本
- 必须在 TCP 连接建立后首先发送（双方可同时发送）

### 5.4 密钥交换流程（以 curve25519-sha256 为例）

```
Client                                 Server
──────                                 ──────
│ SSH_MSG_KEXINIT (算法列表)            │
│─────────────────────────────────────>│
│                                      │
│ SSH_MSG_KEXINIT (算法列表)            │
│<─────────────────────────────────────│
│                                      │
│ SSH_MSG_KEX_ECDH_INIT                │
│  (client_public_key)                 │
│─────────────────────────────────────>│
│                                      │ → 计算 shared_secret K
│                                      │ → 计算 exchange hash H
│                                      │ → 用 host_key 签名 H
│                                      │ → 派生所有密钥
│ SSH_MSG_KEX_ECDH_REPLY               │
│  (host_key, server_public_key,       │
│   signature_of_H)                    │
│<─────────────────────────────────────│
│                                      │
│ SSH_MSG_NEWKEYS                      │
│────────────────────────────────────>│ → Server 激活新密钥
│                                      │
│ SSH_MSG_NEWKEYS                      │
│<─────────────────────────────────────│ → Client 激活新密钥
```

### 5.5 密钥派生

```
shared_secret K (mpint)
exchange_hash H (第一次 H 同时作为 session_id)
session_id = H (后续 rekey 不变)

derive_key(K, H, session_id, letter, key_len):
  if key_len <= hash_len:
    return HASH(K | H | letter | session_id)[:key_len]
  else:
    K1 = HASH(K | H | letter | session_id)
    K2 = HASH(K | H | K1)
    K3 = HASH(K | H | K2)
    return (K1 + K2 + K3 + ...)[:key_len]

letter:
  'A' -> client->server IV
  'B' -> server->client IV
  'C' -> client->server encryption key
  'D' -> server->client encryption key
  'E' -> client->server MAC key
  'F' -> server->client MAC key
```

---

## 6. 算法策略设计

### 6.1 SshKexAlgorithm（密钥交换算法）

```cpp
class SshKexAlgorithm {
public:
    virtual ~SshKexAlgorithm() = default;
    virtual std::string name() const = 0;
    virtual size_t hash_digest_size() const = 0;

    // 生成己方公钥
    virtual std::vector<uint8_t> generate_public_key() = 0;

    // 计算共享密钥
    virtual bool compute_shared_secret(const std::vector<uint8_t>& peer_public,
                                       std::vector<uint8_t>& shared_secret) = 0;

    // 计算 exchange hash
    virtual std::vector<uint8_t> compute_exchange_hash(
        const std::string& client_version,
        const std::string& server_version,
        const std::vector<uint8_t>& client_kex_init,
        const std::vector<uint8_t>& server_kex_init,
        const std::vector<uint8_t>& host_key,
        const std::vector<uint8_t>& client_public,
        const std::vector<uint8_t>& server_public,
        const std::vector<uint8_t>& shared_secret) = 0;

    // 获取己方公钥（generate_public_key 之后调用）
    virtual std::vector<uint8_t> public_key() const = 0;
};
```

### 6.2 SshHostKeyAlgorithm（主机密钥算法）

```cpp
class SshHostKeyAlgorithm {
public:
    virtual ~SshHostKeyAlgorithm() = default;
    virtual std::string name() const = 0;

    // 获取主机公钥（SSH 编码格式）
    virtual std::vector<uint8_t> public_key_blob() const = 0;

    // 签名
    virtual std::vector<uint8_t> sign(const std::vector<uint8_t>& data) = 0;

    // 验证签名
    virtual bool verify(const std::vector<uint8_t>& data,
                       const std::vector<uint8_t>& signature) = 0;

    // 主机密钥指纹
    virtual std::string fingerprint() const = 0;
};
```

### 6.3 SshCipher（加密算法）

```cpp
class SshCipher {
public:
    virtual ~SshCipher() = default;
    virtual std::string name() const = 0;
    virtual size_t block_size() const = 0;
    virtual size_t key_size() const = 0;
    virtual size_t iv_size() const = 0;

    // 初始化（NEWKEYS 后调用）
    virtual void init(const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& iv) = 0;

    // 加密（原地或返回新 buffer）
    virtual std::vector<uint8_t> encrypt(const uint8_t* data, size_t len) = 0;

    // 解密
    virtual std::vector<uint8_t> decrypt(const uint8_t* data, size_t len) = 0;

    // 是否是 AEAD 模式
    virtual bool is_aead() const { return false; }

    // AEAD 模式的 additional data 和 tag 处理
    virtual std::vector<uint8_t> encrypt_aead(const uint8_t* aad, size_t aad_len,
                                              const uint8_t* data, size_t data_len,
                                              const uint8_t* seq_bytes) = 0;
    virtual bool decrypt_aead(const uint8_t* aad, size_t aad_len,
                              const uint8_t* data, size_t data_len,
                              const uint8_t* tag, size_t tag_len,
                              const uint8_t* seq_bytes,
                              std::vector<uint8_t>& out) = 0;
};
```

### 6.4 SshMac（MAC 算法）

```cpp
class SshMac {
public:
    virtual ~SshMac() = default;
    virtual std::string name() const = 0;
    virtual size_t digest_size() const = 0;
    virtual size_t key_size() const = 0;

    virtual void init(const std::vector<uint8_t>& key) = 0;
    virtual std::vector<uint8_t> compute(uint32_t seq,
                                         const uint8_t* data, size_t len) = 0;
    virtual bool verify(uint32_t seq,
                       const uint8_t* data, size_t len,
                       const uint8_t* mac, size_t mac_len) = 0;
};
```

### 6.5 SshCompression（压缩算法）

```cpp
class SshCompression {
public:
    virtual ~SshCompression() = default;
    virtual std::string name() const = 0;

    virtual bool init() = 0;
    virtual std::vector<uint8_t> compress(const uint8_t* data, size_t len) = 0;
    virtual std::vector<uint8_t> decompress(const uint8_t* data, size_t len) = 0;
};
```

### 6.6 算法协商

```cpp
struct SshNegotiatedAlgorithms {
    std::string kex_name;
    std::string host_key_name;
    std::string client_to_server_cipher_name;
    std::string server_to_client_cipher_name;
    std::string client_to_server_mac_name;
    std::string server_to_client_mac_name;
    std::string client_to_server_compression_name;
    std::string server_to_client_compression_name;
};

// 协商规则 (RFC 4253 Section 7.1):
// 第一个算法（客户端或服务端的）必须与对方列表中第一个匹配的算法相同
// 即: 取客户端列表中第一个在服务端列表中也出现的算法
```

### 6.7 首期支持的算法

| 类别 | 算法 | 优先级 |
|------|------|--------|
| KEX | curve25519-sha256 | 1 (最高) |
| KEX | curve25519-sha256@libssh.org | 2 |
| KEX | ecdh-sha2-nistp256 | 3 |
| KEX | ecdh-sha2-nistp384 | 4 |
| KEX | ecdh-sha2-nistp521 | 5 |
| KEX | diffie-hellman-group14-sha256 | 6 |
| KEX | diffie-hellman-group16-sha512 | 7 |
| KEX | diffie-hellman-group18-sha512 | 8 |
| Host Key | ssh-ed25519 | 1 |
| Host Key | ecdsa-sha2-nistp256 | 2 |
| Host Key | ecdsa-sha2-nistp384 | 3 |
| Host Key | rsa-sha2-512 | 4 |
| Host Key | rsa-sha2-256 | 5 |
| Host Key | ssh-rsa | 6 (兼容) |
| Cipher | chacha20-poly1305@openssh.com | 1 |
| Cipher | aes256-gcm@openssh.com | 2 |
| Cipher | aes128-gcm@openssh.com | 3 |
| Cipher | aes256-ctr | 4 |
| Cipher | aes192-ctr | 5 |
| Cipher | aes128-ctr | 6 |
| MAC | hmac-sha2-256 | 1 |
| MAC | hmac-sha2-512 | 2 |
| MAC | hmac-sha1 | 3 (兼容) |
| Compression | none | 1 |
| Compression | zlib@openssh.com | 2 (延迟压缩) |
| Compression | zlib | 3 |

---

## 7. Authentication Layer 设计

### 7.1 认证方法

```cpp
class SshAuthMethod {
public:
    virtual ~SshAuthMethod() = default;
    virtual std::string name() const = 0;

    // 处理认证请求
    // 返回: success / failure / need_more (keyboard-interactive)
    virtual SshAuthResult process(SshSession* session,
                                 const std::vector<uint8_t>& auth_data) = 0;

    // 构造下一个认证请求数据（keyboard-interactive 的 challenge）
    virtual std::vector<uint8_t> build_challenge() { return {}; }
};
```

### 7.2 认证流程

**password:**
```
C -> S: SSH_MSG_USERAUTH_REQUEST(username, "ssh-connection", "password", password)
S -> C: SSH_MSG_USERAUTH_SUCCESS / SSH_MSG_USERAUTH_FAILURE
```

**publickey:**
```
C -> S: SSH_MSG_USERAUTH_REQUEST(username, "ssh-connection", "publickey",
         algo_name, public_key_blob)  [signature=false, 试探]
S -> C: SSH_MSG_USERAUTH_PK_OK(algo_name, public_key_blob)

C -> S: SSH_MSG_USERAUTH_REQUEST(username, "ssh-connection", "publickey",
         algo_name, public_key_blob, signature)  [正式签名]
S -> C: SSH_MSG_USERAUTH_SUCCESS / SSH_MSG_USERAUTH_FAILURE
```

**keyboard-interactive:**
```
C -> S: SSH_MSG_USERAUTH_REQUEST(username, "ssh-connection",
         "keyboard-interactive", language, submethods)
S -> C: SSH_MSG_USERAUTH_INFO_REQUEST(name, instruction, language,
         num_prompts, prompts[])
C -> S: SSH_MSG_USERAUTH_INFO_RESPONSE(num_responses, responses[])
S -> C: SSH_MSG_USERAUTH_SUCCESS / SSH_MSG_USERAUTH_FAILURE /
         SSH_MSG_USERAUTH_INFO_REQUEST (多轮)
```

### 7.3 认证安全约束

- `max_auth_attempts` 次失败后断开连接
- `partial_success` 允许多步认证（如先 password 再 publickey）
- 认证超时保护

---

## 8. Connection Layer 设计

### 8.1 Channel 类型

| 类型 | 用途 | 本期 |
|------|------|------|
| session | 远程执行命令、SFTP | ✅ |
| direct-tcpip | 本地端口转发（客户端请求） | ✅ |
| forwarded-tcpip | 远程端口转发（服务端通知） | ✅ |
| x11 | X11 转发 | ❌ 预留 |

### 8.2 Channel 数据流控

SSH Channel 使用滑动窗口流控：

```
CHANNEL_OPEN:
  sender: initial_window = 2MB, max_packet = 32KB

CHANNEL_DATA:
  sender: 每发送 n 字节，本地 window -= n
  receiver: 收到后本地 window -= n

WINDOW_ADJUST:
  receiver: 当 window < threshold 时发送 WINDOW_ADJUST(bytes_to_add)
  sender: remote_window += bytes_to_add
```

- 初始窗口：2MB（与 OpenSSH 一致）
- 最大包大小：32KB
- 窗口调整阈值：初始窗口的 1/4

### 8.3 全局请求

| 请求 | 说明 | 本期 |
|------|------|------|
| tcpip-forward | 请求服务端监听端口，用于远程转发 | ✅ |
| cancel-tcpip-forward | 取消远程转发 | ✅ |
| hostkeys-00@openssh.com | 主机密钥更新通知 | ❌ |
| no-more-sessions@openssh.com | 禁止新 session channel | ❌ |

### 8.4 端口转发

#### Local Forwarding (direct-tcpip)

```
Client App → SSH Client → SSH Server → Target Server
   │              │             │              │
   │  connect     │             │              │
   │─────────────>│             │              │
   │              │ CH_OPEN     │              │
   │              │(direct-tcpip│              │
   │              │ target:port)│              │
   │              │────────────>│  connect     │
   │              │             │─────────────>│
   │              │ CH_OPEN_OK  │              │
   │              │<────────────│              │
   │  <======== DATA RELAY =========>          │
```

#### Remote Forwarding (forwarded-tcpip)

```
External → SSH Server → SSH Client → Local App
   │            │             │            │
   │  connect   │             │            │
   │───────────>│             │            │
   │            │ CH_OPEN     │            │
   │            │(forwarded-  │            │
   │            │ tcpip)      │            │
   │            │────────────>│  connect   │
   │            │             │───────────>│
   │            │ CH_OPEN_OK  │            │
   │            │<────────────│            │
   │  <======== DATA RELAY =========>      │
```

端口转发的数据中继复用 SOCKS5 的 `relay_pipe()` 协程模式。

---

## 9. SFTP 子系统设计

### 9.1 SFTP 架构

```
┌──────────────────────────────────────┐
│       SshSftpSubsystem               │
│  : SshChannelHandler                 │
│──────────────────────────────────────│
│  - file_system_: SshFileSystem*      │
│  - codec_: SshSftpCodec              │
│  - handles_: map<string, FileHandle> │
│  - next_handle_: uint64_t            │
│──────────────────────────────────────│
│  + on_data(channel, data)            │
│  - process_request(sftp_packet)      │
│  - send_response(id, type, data)     │
└──────────────────────────────────────┘

┌──────────────────────────────────────┐
│       SshFileSystem                  │
│  <<interface>>                       │
│──────────────────────────────────────│
│  + open(path, pflags, attrs)         │
│  + close(handle)                     │
│  + read(handle, offset, len)         │
│  + write(handle, offset, data)       │
│  + lstat(path)                       │
│  + fstat(handle)                     │
│  + setstat(path, attrs)              │
│  + fsetstat(handle, attrs)           │
│  + opendir(path)                     │
│  + readdir(handle)                   │
│  + remove(path)                      │
│  + mkdir(path, attrs)                │
│  + rmdir(path)                       │
│  + realpath(path)                    │
│  + stat(path)                        │
│  + rename(old_path, new_path, flags) │
│  + readlink(path)                    │
│  + symlink(link_path, target_path)   │
│  + extended(subsystem, data)         │
└──────────────────────────────────────┘
```

### 9.2 SFTP 版本支持

首期支持 SFTP v3（最广泛兼容），预留 v4/v5/v6 扩展能力。

### 9.3 SFTP 报文格式

```
┌──────────────────┬──────────────┬─────────────┬──────────┐
│ uint32           │ byte         │ uint32      │ ...      │
│ length           │ type         │ request_id  │ payload  │
└──────────────────┴──────────────┴─────────────┴──────────┘
```

---

## 10. 加密层集成设计

### 10.1 与 Connection 读写路径的集成

SSH 的加密层与 SSL/TLS 的集成方式类似，但有本质区别：

| 维度 | SSL/TLS | SSH |
|------|---------|-----|
| 粒度 | 整个连接 | 逐包 |
| 协商 | TLS Handshake | SSH KEXINIT |
| 切换 | 握手完成一次性切换 | NEWKEYS 消息 |
| Rekey | 通过新握手 | 通过新 KEXINIT |
| 报文格式 | TLS Record | SSH Binary Packet |

SSH 不复用 `SSLHandler` 接口，因为：
1. SSH 的报文格式与 TLS Record 完全不同
2. SSH 需要逐包 sequence number
3. SSH 的 AEAD 模式需要 packet_length 作为 AAD
4. SSH 的 MAC 计算包含 sequence number

SSH 的加密集成在 `SshCipherContext` 中，由 `SshPacketCodec` 调用，而非直接挂载到 `Connection`。

### 10.2 I/O 路径

```
写入路径 (发送):
  SshSession::send(msg_type, payload)
    → SshPacketCodec::encode(seq, payload, cipher_ctx)
      → 明文时: 添加 packet_length + padding_len + padding
      → 加密时: 上述 + cipher_ctx->encrypt() + cipher_ctx->mac()
    → co_await ctx.write_async(encoded_packet)

读取路径 (接收):
  co_await ctx.read_async()
    → SshPacketCodec::try_parse(buf, encrypted, cipher_ctx, seq)
      → 确定完整报文边界
    → SshPacketCodec::decode(seq, data, len, cipher_ctx)
      → 加密时: cipher_ctx->decrypt() + cipher_ctx->verify_mac()
      → 返回明文 payload
    → 分发到对应层处理
```

### 10.3 Rekey 支持

SSH 允许在 active 状态下重新密钥交换（rekey）：

- 任何一方可发起 KEXINIT
- Rekey 期间，数据报文和 KEX 报文交替传输
- 收到 NEWKEYS 前使用旧密钥，收到后切换到新密钥
- Rekey 不影响 session_id（session_id 在首次 KEX 后不变）

---

## 11. 与现有协议的架构对比

| 维度 | SMB | SOCKS5 | SSH |
|------|-----|--------|-----|
| Server 模式 | AsyncListenerHost + coroutine | ConnectionHandler | AsyncListenerHost + coroutine |
| 会话管理 | SmbSessionManager | map<Conn*, Session> | SshSessionManager |
| 加密层 | SmbCrypto (会话级) | 无 | SshCipherContext (逐包级) |
| 算法协商 | 方言协商（有限集合） | 无 | 完整算法协商框架 |
| 通道模型 | TreeConnect（少量） | 无 | Channel（大量、短命） |
| 数据中继 | 无 | relay_pipe | relay_pipe (端口转发) |
| Handler 接口 | SmbHandler (文件操作) | Socks5Handler (认证/控制) | SshHandler (认证/通道/转发) |
| Service 包装 | SmbService | Socks5Service | SshService |
| 状态机复杂度 | 高 | 低 | 最高 |
| 报文格式 | NetBIOS + SMB2 固定头 | 简单 TLV | Binary Packet + 消息类型 |

---

## 12. 文件结构

```
protocol/ssh/
├── CMakeLists.txt
├── include/
│   ├── ssh.h                              # 统一头文件
│   ├── ssh_config.h                       # SshServerConfig
│   ├── ssh_handler.h                      # SshHandler 回调接口
│   ├── ssh_channel_handler.h              # SshChannelHandler 回调接口
│   ├── ssh_server.h                       # SshServer 核心服务器
│   ├── ssh_session.h                      # SshSession 会话
│   ├── ssh_session_manager.h              # 会话管理器
│   │
│   ├── transport/
│   │   ├── ssh_transport.h                # 传输层状态机
│   │   ├── ssh_packet_codec.h             # 二进制包编解码
│   │   ├── ssh_version_exchange.h         # 版本交换
│   │   └── ssh_cipher_context.h           # 加密上下文
│   │
│   ├── auth/
│   │   ├── ssh_authenticator.h            # 认证状态机
│   │   ├── ssh_auth_method.h              # 认证方法接口
│   │   ├── ssh_auth_password.h            # 密码认证
│   │   ├── ssh_auth_publickey.h           # 公钥认证
│   │   └── ssh_auth_keyboard_interactive.h # 键盘交互认证
│   │
│   ├── connection/
│   │   ├── ssh_connection_manager.h       # 连接层管理
│   │   ├── ssh_channel.h                  # 通道对象
│   │   ├── ssh_port_forwarding.h          # 端口转发
│   │   └── ssh_global_request.h           # 全局请求处理
│   │
│   ├── algorithm/
│   │   ├── ssh_algorithm_registry.h       # 算法注册表
│   │   ├── ssh_kex_algorithm.h            # 密钥交换算法接口
│   │   ├── ssh_host_key_algorithm.h       # 主机密钥算法接口
│   │   ├── ssh_cipher.h                   # 加密算法接口
│   │   ├── ssh_mac.h                      # MAC 算法接口
│   │   └── ssh_compression.h              # 压缩算法接口
│   │
│   ├── crypto/
│   │   ├── ssh_crypto.h                   # 加密原语接口
│   │   ├── ssh_crypto_openssl.h           # OpenSSL 实现
│   │   └── ssh_key_derivation.h           # SSH 密钥派生
│   │
│   ├── protocol/
│   │   ├── ssh_constants.h                # 协议常量、消息类型枚举
│   │   ├── ssh_structures.h               # 协议结构体
│   │   └── ssh_message_codec.h            # 消息级编解码（消息类型 + 字段）
│   │
│   ├── sftp/
│   │   ├── ssh_sftp_subsystem.h           # SFTP 子系统
│   │   ├── ssh_sftp_codec.h               # SFTP 报文编解码
│   │   └── ssh_file_system.h              # 文件系统抽象接口
│   │
│   └── hostkey/
│       └── ssh_host_key_provider.h        # 主机密钥管理
│
└── src/
    ├── ssh_server.cpp
    ├── ssh_session.cpp
    ├── ssh_session_manager.cpp
    │
    ├── transport/
    │   ├── ssh_transport.cpp
    │   ├── ssh_packet_codec.cpp
    │   ├── ssh_version_exchange.cpp
    │   └── ssh_cipher_context.cpp
    │
    ├── auth/
    │   ├── ssh_authenticator.cpp
    │   ├── ssh_auth_password.cpp
    │   ├── ssh_auth_publickey.cpp
    │   └── ssh_auth_keyboard_interactive.cpp
    │
    ├── connection/
    │   ├── ssh_connection_manager.cpp
    │   ├── ssh_channel.cpp
    │   ├── ssh_port_forwarding.cpp
    │   └── ssh_global_request.cpp
    │
    ├── algorithm/
    │   ├── ssh_algorithm_registry.cpp
    │   ├── kex/
    │   │   ├── ssh_kex_curve25519.cpp
    │   │   ├── ssh_kex_ecdh_nistp.cpp
    │   │   └── ssh_kex_dh_group.cpp
    │   ├── hostkey/
    │   │   ├── ssh_hk_ed25519.cpp
    │   │   ├── ssh_hk_ecdsa.cpp
    │   │   └── ssh_hk_rsa.cpp
    │   ├── cipher/
    │   │   ├── ssh_cipher_chacha20_poly1305.cpp
    │   │   ├── ssh_cipher_aes_gcm.cpp
    │   │   └── ssh_cipher_aes_ctr.cpp
    │   ├── mac/
    │   │   ├── ssh_mac_hmac_sha2.cpp
    │   │   └── ssh_mac_hmac_sha1.cpp
    │   └── compression/
    │       └── ssh_compression_zlib.cpp
    │
    ├── crypto/
    │   ├── ssh_crypto_openssl.cpp
    │   └── ssh_key_derivation.cpp
    │
    ├── protocol/
    │   └── ssh_message_codec.cpp
    │
    ├── sftp/
    │   ├── ssh_sftp_subsystem.cpp
    │   ├── ssh_sftp_codec.cpp
    │   └── ssh_file_system.cpp
    │
    └── hostkey/
        └── ssh_host_key_provider.cpp

server/services/
├── include/
│   └── ssh_service.h                      # SshService 服务包装
└── src/
    └── ssh_service.cpp                    # 服务包装实现
```

---

## 13. 核心流程伪代码

### 13.1 服务器主循环

```cpp
coroutine::Task<void> SshServer::handle_connection(AsyncConnectionContext ctx) {
    SshSession session(ctx, this);

    // 1. 版本交换
    auto version_result = co_await session.transport().exchange_version(ctx);
    if (!version_result) {
        co_return;
    }

    ByteBuffer recv_buf;

    while (session.state() != SshSession::State::disconnected) {
        auto read_result = co_await ctx.read_async(config_.idle_timeout_ms);
        if (read_result.status != coroutine::IoStatus::success) {
            break;
        }
        recv_buf.append(read_result.data);

        // 2. 循环解析完整报文
        while (recv_buf.readable_bytes() > 0) {
            auto parse = SshPacketCodec::try_parse(
                recv_buf,
                session.transport().is_encrypted(),
                session.transport().cipher_context(),
                session.transport().recv_seq());

            if (!parse.complete) break;

            // 3. 解码报文
            auto payload = SshPacketCodec::decode(
                session.transport().recv_seq(),
                recv_buf.read_ptr(), parse.total_bytes,
                session.transport().cipher_context());
            session.transport().increment_recv_seq();

            if (!payload) {
                session.send_disconnect(SSH_DISCONNECT_PROTOCOL_ERROR, "decrypt failed");
                co_return;
            }

            recv_buf.consume(parse.total_bytes);

            // 4. 分发到对应层
            auto msg_type = static_cast<SshMessageType>((*payload)[0]);
            session.dispatch(msg_type, *payload);
        }
    }

    session_mgr_.remove_session(session.session_id());
}
```

### 13.2 分发逻辑

```cpp
void SshSession::dispatch(SshMessageType msg_type, const std::vector<uint8_t>& payload) {
    switch (state_) {
    case State::version_exchanged:
    case State::kex_init:
    case State::kex_in_progress:
    case State::newkeys:
        transport_.process_message(msg_type, payload);
        break;

    case State::auth_start:
    case State::authenticating:
        if (msg_type == SshMessageType::SERVICE_REQUEST) {
            handle_service_request(payload);
        } else if (msg_type == SshMessageType::USERAUTH_REQUEST) {
            authenticator_.process_userauth_request(payload);
        }
        break;

    case State::active:
        if (msg_type == SshMessageType::KEXINIT) {
            // Rekey
            transport_.process_message(msg_type, payload);
        } else {
            conn_mgr_.process_message(msg_type, payload);
        }
        break;

    default:
        break;
    }
}
```

### 13.3 端口转发中继

```cpp
coroutine::Task<void> SshPortForwarding::relay_pipe(
    SshChannel& ssh_channel,
    AsyncConnectionContext target_ctx) {

    // 方向 1: SSH Channel → Target
    auto forward_to_target = [&]() -> coroutine::Task<void> {
        while (ssh_channel.state() == SshChannel::State::open) {
            auto data = co_await ssh_channel.read_async();
            if (data.empty()) break;
            co_await target_ctx.write_async(std::move(data));
        }
    };

    // 方向 2: Target → SSH Channel
    auto forward_to_ssh = [&]() -> coroutine::Task<void> {
        while (ssh_channel.state() == SshChannel::State::open) {
            auto read_result = co_await target_ctx.read_async();
            if (read_result.status != coroutine::IoStatus::success) break;
            ssh_channel.write_data(read_result.data);
        }
    };

    co_await coroutine::when_all(forward_to_target(), forward_to_ssh());
}
```

---

## 14. 构建系统集成

### 14.1 新增 CMake 目标

- `SshProto`：SSH 协议库，链接 `Core` + `OpenSSL`
- `ServerServices`：新增依赖 `SshProto`

### 14.2 新增第三方依赖

| 依赖 | 用途 | 集成方式 |
|------|------|---------|
| zlib | SSH 压缩 (zlib / zlib@openssh.com) | git submodule → `third_party/zlib` |

### 14.3 CMake 选项

- `YUAN_ENABLE_SSH`：启用 SSH 协议（ON by default）
- `YUAN_ENABLE_SSH_SFTP`：启用 SFTP 子系统（ON by default）

### 14.4 修改的 CMakeLists.txt

| 文件 | 变更 |
|------|------|
| `CMakeLists.txt` | 新增 `add_subdirectory(protocol/ssh)`，Test 链接 `SshProto` |
| `protocol/ssh/CMakeLists.txt` | 新建，定义 `SshProto` 库 |
| `server/services/CMakeLists.txt` | `target_link_libraries` 新增 `SshProto` |

---

## 15. 服务注册

```cpp
#include "ssh_service.h"

yuan::net::ssh::SshServerConfig ssh_config;
ssh_config.port = 22;
ssh_config.host_key_paths = {"/etc/ssh/ssh_host_ed25519_key"};
ssh_config.auth_methods = {"publickey", "password"};
ssh_config.enable_sftp = true;

application.add_typed_service<yuan::server::SshService>(
    "ssh",
    std::make_shared<yuan::server::SshService>(ssh_config),
    "server.ssh",
    1);
```

---

## 16. 测试策略

### 16.1 单元测试

| 模块 | 测试内容 |
|------|---------|
| SshPacketCodec | 报文编解码（明文/加密/AEAD） |
| SshMessageCodec | 各消息类型编解码 |
| SshKeyDerivation | 密钥派生正确性 |
| SshCrypto | 加解密/MAC/DH/ECDH |
| SshAlgorithmRegistry | 算法注册/协商 |
| SshVersionExchange | 版本字符串解析 |
| SshChannel | 通道状态机/窗口流控 |
| SshSftpCodec | SFTP 报文编解码 |
| SshAuthenticator | 认证状态机 |

### 16.2 集成测试

| 场景 | 测试方式 |
|------|---------|
| 完整握手 | 使用 OpenSSH 客户端连接 |
| 密码认证 | `ssh -o PreferredAuthentications=password` |
| 公钥认证 | `ssh -i ~/.ssh/id_ed25519` |
| 执行命令 | `ssh user@host "ls -la"` |
| SFTP | `sftp user@host` + get/put/ls |
| 本地端口转发 | `ssh -L 8080:target:80` + curl |
| 远程端口转发 | `ssh -R 9090:local:90` |
| Rekey | 发送大量数据触发 rekey |
| 算法协商 | 指定不同算法组合 |
| 错误处理 | 非法报文/认证失败/通道关闭 |

### 16.3 手动测试

```bash
# 基本连接
ssh -v -p 22 user@localhost

# 指定算法
ssh -o KexAlgorithms=curve25519-sha256 \
    -o Ciphers=aes256-gcm@openssh.com \
    -o MACs=hmac-sha2-256 \
    user@localhost

# SFTP
sftp user@localhost

# 端口转发
ssh -L 8080:127.0.0.1:80 user@localhost
ssh -R 9090:127.0.0.1:90 user@localhost
```
