# SMB 协议实现设计文档

## 1. 概述

本文档描述基于 `server-lib` 架构新增 SMB（Server Message Block）协议的完整实现设计。SMB 是 Windows 文件共享的核心协议，也是 NAS、域控、企业文件服务的基石。本实现覆盖 SMB2/SMB3 协议族（MS-SMB2），兼容 SMB1/CIFS（MS-CIFS）的基础协商，支持完整的文件共享、认证、加锁、Oplock/Lease、命名管道、DFS 等特性。

### 1.1 参考规范

| 规范 | 说明 |
|------|------|
| MS-SMB2 | SMB2/SMB3 Protocol |
| MS-CIFS | Common Internet File System |
| MS-SPNG | SPNEGO Protocol |
| MS-NLMP | NTLM Authentication Protocol |
| MS-FSCC | File System Control Codes |
| MS-DFSC | Distributed File System |
| MS-RPCE | Remote Procedure Call Extensions |
| MS-SRVS | Server Service Remote Protocol |
| MS-WKST | Workstation Service Remote Protocol |

## 2. 设计目标

- **协议完整性**：覆盖 SMB2/SMB3 的所有核心命令和重要可选命令
- **性能优先**：零拷贝传输、多信用（Credits）机制、复合请求（Compound）、异步通知
- **安全性**：SMB3 加密（AES-128-CCM/GCM）、签名（HMAC-SHA256/AES-CMAC）、预认证完整性
- **可扩展**：Handler 回调接口支持自定义认证/授权/文件系统/管道处理
- **框架适配**：复用 `server-lib` 的 Reactor + Coroutine 异步模型

## 3. 架构设计

### 3.1 整体分层

```
┌─────────────────────────────────────────────────────────────┐
│                   Application (main.cpp)                     │
│    add_typed_service<SmbService>(...)                        │
├─────────────────────────────────────────────────────────────┤
│                    Service Layer                              │
│    SmbService : Service                                      │
│              : RuntimeContextAwareService                     │
│    - 持有 SmbServer (unique_ptr)                             │
│    - 独立线程运行 NetworkRuntime                             │
│    - 通过 EventBus 发布生命周期事件                           │
├─────────────────────────────────────────────────────────────┤
│                    Protocol Layer                             │
│    ┌───────────────────────────────────────────────┐         │
│    │ SmbServer                                     │         │
│    │  - AsyncListenerHost 监听 445                  │         │
│    │  - SmbSessionManager 会话管理                   │         │
│    │  - SmbDispatcher 命令分发                       │         │
│    │  - SmbShareManager 共享管理                     │         │
│    └───────────────────────────────────────────────┘         │
│    ┌──────────────────┐  ┌──────────────────────┐            │
│    │ Smb2Codec        │  │ Smb1Codec (negotiate) │            │
│    │ SMB2 报文编解码   │  │ SMB1 兼容协商         │            │
│    └──────────────────┘  └──────────────────────┘            │
│    ┌──────────────────┐  ┌──────────────────────┐            │
│    │ SmbAuth          │  │ SmbCrypto             │            │
│    │ NTLMv2/SPNEGO    │  │ AES-CCM/GCM/签名      │            │
│    └──────────────────┘  └──────────────────────┘            │
│    ┌──────────────────┐  ┌──────────────────────┐            │
│    │ SmbFileSystem    │  │ SmbPipeManager        │            │
│    │ 文件系统抽象      │  │ 命名管道与 RPC         │            │
│    └──────────────────┘  └──────────────────────┘            │
│    ┌──────────────────┐  ┌──────────────────────┐            │
│    │ SmbLockManager   │  │ SmbDfsResolver        │            │
│    │ 锁/Oplock/Lease  │  │ DFS 路径解析           │            │
│    └──────────────────┘  └──────────────────────┘            │
├─────────────────────────────────────────────────────────────┤
│                    Core Layer                                │
│    Poller / EventLoop / Acceptor / Connection                │
│    AsyncListenerHost / AsyncConnectionContext                 │
│    ByteBuffer / Coroutine / TimerManager                     │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 核心类图

```
┌─────────────────────────────┐
│      SmbServerConfig        │
│  - port (default 445)       │
│  - server_name              │
│  - domain_name              │
│  - enable_smb1_fallback     │
│  - enable_encryption        │
│  - require_signing          │
│  - max_sessions             │
│  - max_credits              │
│  - idle_timeout_ms          │
│  - auth_methods             │
│  - shares                   │
└─────────────────────────────┘
          │
          ▼
┌─────────────────────────────────────────────┐
│               SmbServer                     │
│─────────────────────────────────────────────│
│  - listener_: AsyncListenerHost             │
│  - session_mgr_: SmbSessionManager          │
│  - dispatcher_: SmbDispatcher               │
│  - share_mgr_: SmbShareManager              │
│  - handler_: SmbHandler*                    │
│  - crypto_ctx_: SmbCryptoContext            │
│─────────────────────────────────────────────│
│  + init(port) / serve() / stop()            │
│  + set_handler(SmbHandler*)                 │
│  - handle_connection(AsyncConnCtx)          │
│  - process_smb_message(SmbSession&, buf)    │
└─────────────────────────────────────────────┘
          │ 1
          │
          │ *
          ▼
┌─────────────────────────────────────────────┐
│              SmbSession                     │
│─────────────────────────────────────────────│
│  - conn_: Connection*                       │
│  - ctx_: AsyncConnectionContext             │
│  - state_: State                            │
│  - dialect_: DialectRevision                │
│  - session_id_: uint64_t                    │
│  - tree_connects_: map<uint32, TreeConn>    │
│  - credits_granted_: uint16_t               │
│  - credits_consumed_: uint16_t              │
│  - signing_key_: vector<uint8_t>            │
│  - encryption_key_: vector<uint8_t>         │
│  - preauth_hash_: vector<uint8_t>           │
│  - lock_mgr_: SmbLockManager               │
│  - oplock_state_: OplockState               │
│─────────────────────────────────────────────│
│  + negotiate()                              │
│  + session_setup()                          │
│  + tree_connect()                           │
│  + close()                                  │
│  + grant_credits(n) / consume_credits(n)    │
│  + is_encrypted() / is_signed()             │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│              SmbHandler                     │
│  <<interface>>                              │
│─────────────────────────────────────────────│
│  + on_authenticate(session, user, domain)   │
│  + on_tree_connect(session, path)           │
│  + on_tree_disconnect(session, tree_id)     │
│  + on_create(session, tree, path, access)   │
│  + on_close(session, file_id)               │
│  + on_read(session, file_id, offset, len)   │
│  + on_write(session, file_id, offset, data) │
│  + on_query_directory(session, file_id)     │
│  + on_query_info(session, file_id)          │
│  + on_set_info(session, file_id)            │
│  + on_rename(session, file_id, new_path)    │
│  + on_delete(session, file_id)              │
│  + on_lock(session, file_id, locks)         │
│  + on_ioctl(session, file_id, code)         │
│  + on_pipe_open(session, pipe_name)         │
│  + on_pipe_read/write(session, file_id)     │
│  + on_session_opened(session)               │
│  + on_session_closed(session)               │
│  + on_logoff(session)                       │
│  + on_dfs_resolve(session, path)            │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│             Smb2Codec                       │
│  <<static utility>>                         │
│─────────────────────────────────────────────│
│  + encode_header(Smb2Header) -> ByteBuffer  │
│  + decode_header(ByteBuffer) -> Smb2Header  │
│  + encode_negotiate_request/response        │
│  + encode_session_setup_request/response    │
│  + encode_tree_connect_request/response     │
│  + encode_create_request/response           │
│  + encode_read_request/response             │
│  + encode_write_request/response            │
│  + encode_close_request/response            │
│  + encode_query_directory_req/resp          │
│  + encode_query_info_req/resp               │
│  + encode_set_info_req/resp                 │
│  + encode_lock_request/response             │
│  + encode_ioctl_request/response            │
│  + encode_change_notify_req/resp            │
│  + encode_oplock_break_ack/notification     │
│  + encode_lease_break_ack/notification      │
│  + encode_echo_request/response             │
│  + encode_logoff_request/response           │
│  + encode_transform_header(encrypted)       │
│  + decode_transform_header                  │
│  + build_error_response(status)             │
│  + validate_signature(buf, key)             │
│  + calculate_signature(buf, key)            │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│              SmbAuth                        │
│─────────────────────────────────────────────│
│  + ntlmssp_negotiate(flags) -> ByteBuffer   │
│  + ntlmssp_challenge(server_challenge)      │
│  + ntlmssp_authenticate(token) -> result    │
│  + ntlmv2_response(server, user, pass,      │
│      challenge, target_info) -> hash        │
│  + spnego_init(mech_types) -> token         │
│  + spnego_negotiate(token) -> result        │
│  + derive_signing_key(session_key)          │
│  + derive_encryption_key(session_key)       │
│  + derive_decryption_key(session_key)       │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│             SmbCrypto                       │
│─────────────────────────────────────────────│
│  + sign_aes_cmac(key, buf) -> signature     │
│  + sign_hmac_sha256(key, buf) -> sig        │
│  + encrypt_aes_ccm(key, nonce, buf)         │
│  + decrypt_aes_ccm(key, nonce, buf)         │
│  + encrypt_aes_gcm(key, nonce, buf)         │
│  + decrypt_aes_gcm(key, nonce, buf)         │
│  + preauth_hash_sha512(data) -> hash        │
│  + derive_key_using_kdf(key, label, ctx)    │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│           SmbFileSystem                     │
│  <<interface>>                              │
│─────────────────────────────────────────────│
│  + open(path, access, disposition, create)  │
│  + close(file_handle)                       │
│  + read(file_handle, offset, length)        │
│  + write(file_handle, offset, data)         │
│  + query_info(file_handle, info_class)      │
│  + set_info(file_handle, info_class, data)  │
│  + query_directory(file_handle, pattern)    │
│  + rename(file_handle, new_path)            │
│  + delete(file_handle)                      │
│  + flush(file_handle)                       │
│  + lock(file_handle, offset, length, excl)  │
│  + unlock(file_handle, offset, length)      │
│  + fsctl(file_handle, code, input)          │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│          SmbShareManager                    │
│─────────────────────────────────────────────│
│  - shares_: map<string, SmbShare>           │
│─────────────────────────────────────────────│
│  + add_share(SmbShare)                      │
│  + remove_share(name)                       │
│  + find_share(name) -> SmbShare*            │
│  + list_shares() -> vector<SmbShare>        │
│  + resolve_path(share, relative) -> path    │
└─────────────────────────────────────────────┘

┌─────────────────────────────┐
│         SmbShare            │
│─────────────────────────────│
│  - name: string             │
│  - comment: string          │
│  - type: ShareType          │
│    disk / pipe / print      │
│  - path: string (root dir)  │
│  - password: string         │
│  - max_uses: int            │
│  - current_uses: int        │
│  - file_system: SmbFileSystem* │
│  - permissions: SharePerms  │
└─────────────────────────────┘

┌─────────────────────────────────────────────┐
│          SmbLockManager                     │
│─────────────────────────────────────────────│
│  - locks_: map<FileId, vector<SmbLock>>     │
│  - oplocks_: map<FileId, OplockEntry>      │
│  - leases_: map<LeaseKey, LeaseEntry>      │
│─────────────────────────────────────────────│
│  + request_lock(file, offset, len, excl)    │
│  + release_lock(file, offset, len)          │
│  + request_oplock(file, level)              │
│  + ack_oplock_break(file, level)            │
│  + request_lease(key, state)               │
│  + break_lease(key, new_state)             │
│  + check_range(file, offset, len) -> bool   │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│         SmbPipeManager                     │
│─────────────────────────────────────────────│
│  - pipes_: map<string, SmbPipe>             │
│─────────────────────────────────────────────│
│  + register_pipe(name, handler)             │
│  + open_pipe(name) -> PipeHandle            │
│  + read_pipe(handle, len) -> data           │
│  + write_pipe(handle, data)                 │
│  + close_pipe(handle)                       │
│  + list_pipes() -> vector<string>           │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│          SmbDispatcher                      │
│─────────────────────────────────────────────│
│  - handlers_: map<Command, CmdHandler>      │
│─────────────────────────────────────────────│
│  + dispatch(session, header, buf) -> resp   │
│  + register_handler(cmd, fn)                │
│  + dispatch_compound(session, chain)        │
│  + dispatch_interim(session, header)        │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│          SmbDfsResolver                     │
│─────────────────────────────────────────────│
│  - referrals_: map<string, DfsReferral>     │
│─────────────────────────────────────────────│
│  + resolve(path) -> DfsReferral             │
│  + add_referral(path, target)               │
│  + remove_referral(path)                    │
└─────────────────────────────────────────────┘
```

### 3.3 TreeConnect（共享连接）

```
┌─────────────────────────────┐
│        TreeConnection       │
│─────────────────────────────│
│  - tree_id: uint32_t        │
│  - share_name: string       │
│  - share: SmbShare*         │
│  - file_system: SmbFileSystem*│
│  - open_files: map<FileId,  │
│      OpenFile>              │
│  - is_dfs: bool             │
│  - is_ca: bool              │
└─────────────────────────────┘

┌─────────────────────────────┐
│        OpenFile             │
│─────────────────────────────│
│  - file_id: FileId          │
│  - persistent_id: uint64_t  │
│  - path: string             │
│  - access_mask: uint32_t    │
│  - share_access: uint32_t   │
│  - create_disposition       │
│  - attributes: uint32_t     │
│  - is_directory: bool       │
│  - oplock_level: uint8_t    │
│  - lease_key: LeaseKey      │
│  - lease_state: uint32_t    │
│  - file_handle: void*       │
│  - durable: bool            │
│  - durable_timeout_ms       │
└─────────────────────────────┘
```

## 4. 协议状态机

### 4.1 连接级状态机

```
  Client                                Server
  ──────                                ──────
  │  TCP Connect (port 445)             │
  │────────────────────────────────────>│  State: connected
  │                                     │
  │  NegProt Request                    │
  │  (SMB1: 0xFF + dialect strings)    │
  │────────────────────────────────────>│  State: negotiating
  │                                     │  → 选择最高支持方言
  │  NegProt Response                   │
  │  (SMB2 header + negotiate resp)    │
  │<────────────────────────────────────│
  │                                     │
  │  Session Setup Request              │
  │  (SPNEGO/NTLMSSP negotiate token)  │
  │────────────────────────────────────>│  State: authenticating
  │                                     │
  │  Session Setup Response             │
  │  (SPNEGO/NTLMSSP challenge token)  │
  │<────────────────────────────────────│
  │                                     │
  │  Session Setup Request              │
  │  (SPNEGO/NTLMSSP auth token)       │
  │────────────────────────────────────>│
  │                                     │
  │  Session Setup Response             │
  │  (STATUS_SUCCESS + session_id)      │
  │<────────────────────────────────────│  State: authenticated
  │                                     │
  │  Tree Connect Request               │
  │  (\\server\share)                   │
  │────────────────────────────────────>│  State: tree_connected
  │                                     │
  │  Tree Connect Response              │
  │  (tree_id + share type)             │
  │<────────────────────────────────────│
  │                                     │
  │  Create / Read / Write / Close ...  │
  │<═══════════════════════════════════>│  State: active
  │                                     │
  │  Tree Disconnect                    │
  │────────────────────────────────────>│
  │                                     │
  │  Logoff                             │
  │────────────────────────────────────>│  State: logged_off
  │                                     │
  │  TCP Close                          │
  │────────────────────────────────────>│  State: closed
```

### 4.2 状态转换表

| 当前状态 | 事件 | 动作 | 下一状态 |
|---------|------|------|---------|
| connected | NEGOTIATE | 解析方言列表，选择最高版本，发送协商响应 | negotiating |
| negotiating | SESSION_SETUP | 初始化认证上下文，发送 NTLMSSP Challenge | authenticating |
| authenticating | SESSION_SETUP (续) | 验证 NTLMv2 响应，派生密钥，发送成功 | authenticated |
| authenticated | TREE_CONNECT | 验证共享路径，创建 TreeConnection | active |
| active | CREATE | 打开文件，分配 FileId，返回句柄 | active |
| active | READ | 读取文件数据，返回内容 | active |
| active | WRITE | 写入文件数据，返回写入字节数 | active |
| active | CLOSE | 关闭文件，释放锁/Oplock | active |
| active | TREE_DISCONNECT | 关闭所有打开文件，释放 TreeConnection | authenticated |
| active | LOGOFF | 清除所有 TreeConnection 和 Session | connected |
| any | EOF / TCP close | 清理所有资源 | closed |

## 5. SMB2 协议编解码设计

### 5.1 SMB2 消息帧格式

```
SMB2 消息帧:
┌─────────────────────────────┐
│  TCP NetBIOS Header (4B)    │  Session Service: 0x00 + 3字节长度
├─────────────────────────────┤
│  SMB2 Header (64B)          │  固定64字节头
├─────────────────────────────┤
│  SMB2 Command Payload       │  变长，取决于 Command
├─────────────────────────────┤
│  [Padding / Buffer]         │  对齐填充和动态缓冲区
└─────────────────────────────┘

加密消息帧 (SMB 3.x):
┌─────────────────────────────┐
│  TCP NetBIOS Header (4B)    │
├─────────────────────────────┤
│  SMB2 Transform Header(52B) │  0xFD00 + nonce + enc_data
├─────────────────────────────┤
│  Encrypted SMB2 Message     │  AES-128-CCM/GCM 加密
└─────────────────────────────┘

复合请求 (Compound):
┌─────────────────────────────┐
│  NetBIOS Header             │
├─────────────────────────────┤
│  SMB2 Header #1 (64B)      │  NextCommand = offset_to_next
├─────────────────────────────┤
│  Command #1 Payload         │
├─────────────────────────────┤
│  SMB2 Header #2 (64B)      │  NextCommand = 0 (最后一个)
├─────────────────────────────┤
│  Command #2 Payload         │
└─────────────────────────────┘
```

### 5.2 SMB2 Header 结构 (64 字节)

```cpp
struct Smb2Header {
    uint32_t protocol_id;       // 0xFE, 'S', 'M', 'B'
    uint16_t structure_size;    // 64
    uint16_t credit_charge;     // 消耗的信用数
    uint32_t status;            // NT Status
    uint16_t command;           // SMB2 命令
    uint16_t credit_request;    // 请求的信用数
    uint32_t flags;             // SMB2_FLAGS_*
    uint32_t next_command;      // 复合请求中下一命令偏移
    uint64_t message_id;        // 消息 ID
    uint32_t reserved;          // 保留
    uint32_t tree_id;           // Tree Connect ID
    uint64_t session_id;        // Session ID
    uint8_t  signature[16];     // 签名
};
```

### 5.3 SMB2 命令枚举

| 命令 | 值 | 说明 |
|------|-----|------|
| NEGOTIATE | 0x0000 | 协议协商 |
| SESSION_SETUP | 0x0001 | 会话建立 |
| LOGOFF | 0x0002 | 注销 |
| TREE_CONNECT | 0x0003 | 树连接 |
| TREE_DISCONNECT | 0x0004 | 树断开 |
| CREATE | 0x0005 | 创建/打开文件 |
| CLOSE | 0x0006 | 关闭文件 |
| FLUSH | 0x0007 | 刷新 |
| READ | 0x0008 | 读取 |
| WRITE | 0x0009 | 写入 |
| LOCK | 0x000A | 锁定 |
| IOCTL | 0x000B | IO 控制 |
| CANCEL | 0x000C | 取消 |
| ECHO | 0x000D | 回声 |
| QUERY_DIRECTORY | 0x000E | 查询目录 |
| CHANGE_NOTIFY | 0x000F | 变更通知 |
| QUERY_INFO | 0x0010 | 查询信息 |
| SET_INFO | 0x0011 | 设置信息 |
| OPLOCK_BREAK | 0x0012 | Oplock 中断 |

### 5.4 SMB2 方言版本

| 方言 | 值 | 关键特性 |
|------|-----|---------|
| SMB 2.002 | 0x0202 | 基础 SMB2 |
| SMB 2.1 | 0x0210 | Oplock/Lease, 大 MTU |
| SMB 3.0 | 0x0300 | 加密, 多通道, 持久句柄 |
| SMB 3.0.2 | 0x0302 | 3.0 修复 |
| SMB 3.1.1 | 0x0311 | 预认证完整性, AES-GCM |

### 5.5 编解码策略

- **零拷贝读取**：使用 `ByteBuffer::readable_span()` 获取内存视图，避免数据拷贝
- **流式解码**：`Smb2Decoder` 从 `ByteBuffer` 顺序读取字段，自动处理字节序（SMB2 为小端序）
- **增量编码**：`Smb2Encoder` 往 `ByteBuffer` 写入字段，预计算结构大小避免重分配
- **小端序注意**：SMB2 协议使用小端序，与网络字节序（大端）不同。ByteBuffer 的 `read_u16/read_u32` 等方法做了网络序转换，SMB 需要直接内存拷贝或使用小端序读取

### 5.6 小端序支持

由于 SMB2 协议全部使用小端序，我们需要在 `ByteBuffer` 基础上提供小端序读写能力：

```cpp
// 在 Smb2Codec 中使用直接内存操作处理小端序
class Smb2Codec {
    // 从 ByteBuffer 读取小端序值
    static uint16_t read_le16(const uint8_t* p) {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }
    static uint32_t read_le32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
             | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
    static uint64_t read_le64(const uint8_t* p);
    
    // 向 ByteBuffer 写入小端序值
    static void write_le16(ByteBuffer& buf, uint16_t v);
    static void write_le32(ByteBuffer& buf, uint32_t v);
    static void write_le64(ByteBuffer& buf, uint64_t v);
};
```

## 6. 认证设计

### 6.1 NTLMv2 认证流程

```
Client                          Server
──────                          ──────
│ Session Setup Request         │
│  SecurityBlob =               │
│   SPNEGO(NegTokenInit)        │
│   NTLMSSP(Type1: Negotiate)  │
│──────────────────────────────>│
│                               │ → 生成 Server Challenge (8字节随机)
│ Session Setup Response        │ → 构造 TargetInfo
│  STATUS_MORE_PROCESSING       │ → 计算 NTLMv2 Session Key
│  SecurityBlob =               │
│   SPNEGO(NegTokenResp)        │
│   NTLMSSP(Type2: Challenge)  │
│<──────────────────────────────│
│                               │
│ Session Setup Request         │
│  SecurityBlob =               │
│   SPNEGO(NegTokenResp)        │
│   NTLMSSP(Type3: Auth)       │
│   NTProofStr + RestBlob       │
│──────────────────────────────>│
│                               │ → 验证 NTProofStr
│                               │ → 派生 Session Key
│ Session Setup Response        │ → 派生 Signing/Encryption Key
│  STATUS_SUCCESS               │
│<──────────────────────────────│
```

### 6.2 密钥派生

```
Session Key (NTLMv2)
    │
    ├── SMB 2.x:
    │   └── Signing Key = HMAC-SHA256(SessionKey, "SMB2APPKEY" + SessionId)
    │
    ├── SMB 3.0/3.0.2:
    │   ├── Signing Key = SP800-108(SessionKey, "SmbSign" + Context)
    │   ├── Encryption Key = SP800-108(SessionKey, "SmbRCEnc" + Context)
    │   └── Decryption Key = SP800-108(SessionKey, "SmbRCDec" + Context)
    │
    └── SMB 3.1.1:
        ├── Preauth Hash = SHA-512(累积的协商+认证消息)
        ├── Signing Key = KDF(SessionKey, "SMBSigningKey" + PreauthHash)
        ├── Encryption Key = KDF(SessionKey, "SMBC2SCipherKey" + PreauthHash)
        ├── Decryption Key = KDF(SessionKey, "SMBS2CCipherKey" + PreauthHash)
        └── Encryption/Decryption IV = KDF(SessionKey, "SMBC2SIV/SMBS2CIV" + PreauthHash)
```

## 7. 文件系统抽象设计

### 7.1 SmbFileSystem 接口

提供统一的文件系统操作接口，默认实现基于本地文件系统，可通过 Handler 覆盖：

```cpp
class SmbFileSystem {
public:
    virtual ~SmbFileSystem() = default;
    
    virtual OpenResult open(const std::string& path, 
                           uint32_t desired_access,
                           uint32_t create_disposition,
                           uint32_t create_options) = 0;
    virtual void close(FileHandle handle) = 0;
    virtual ReadResult read(FileHandle handle, uint64_t offset, uint32_t length) = 0;
    virtual WriteResult write(FileHandle handle, uint64_t offset, 
                             const uint8_t* data, uint32_t length) = 0;
    virtual InfoResult query_info(FileHandle handle, FileInfoClass info_class) = 0;
    virtual int set_info(FileHandle handle, FileInfoClass info_class, const uint8_t* data, uint32_t len) = 0;
    virtual DirResult query_directory(FileHandle handle, const std::string& pattern, 
                                      FileInfoClass info_class, bool restart) = 0;
    virtual int rename(FileHandle handle, const std::string& new_path, bool replace) = 0;
    virtual int delete_file(FileHandle handle) = 0;
    virtual int flush(FileHandle handle) = 0;
    virtual LockResult lock(FileHandle handle, uint64_t offset, uint64_t length, 
                           bool exclusive, bool blocking) = 0;
    virtual int unlock(FileHandle handle, uint64_t offset, uint64_t length) = 0;
    virtual IoctlResult fsctl(FileHandle handle, uint32_t code, 
                             const uint8_t* input, uint32_t input_len) = 0;
};
```

### 7.2 本地文件系统实现

`LocalFileSystem` 实现基于 POSIX API 的本地文件操作：
- `open()` → `::open()` with O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/O_TRUNC etc.
- `read()` → `pread64()` 零拷贝直接读取到 ByteBuffer
- `write()` → `pwrite64()` 
- `lock()` → `fcntl(F_SETLK/F_SETLKW)` POSIX 文件锁
- 目录遍历 → `std::filesystem::directory_iterator`

### 7.3 虚拟文件系统实现

`VirtualFileSystem` 实现内存中的虚拟文件系统，用于测试和特殊共享（如 IPC$）。

## 8. 锁与 Oplock/Lease 设计

### 8.1 锁机制

```
SmbLockManager:
  ├─ Byte-Range Locks: 按 (FileId, Offset, Length) 组织
  │   排他锁与共享锁的兼容性矩阵检查
  │   阻塞锁请求入队列等待
  │
  ├─ Oplock (SMB 2.1):
  │   Level 1 (Exclusive)  → 允许缓存读+写
  │   Level 2 (Shared)     → 允许缓存读
  │   Batch                 → 允许缓存读+写+句柄
  │
  └─ Lease (SMB 3.x):
      Lease Key: 客户端提供的 16 字节标识
      Lease State:
        SMB2_LEASE_READ_CACHING    (0x01) - 读缓存
        SMB2_LEASE_HANDLE_CACHING  (0x02) - 句柄缓存
        SMB2_LEASE_WRITE_CACHING   (0x04) - 写缓存
      Break 流程:
        服务器 → 客户端: Lease Break Notification
        客户端 → 服务器: Lease Break Acknowledgment
```

### 8.2 Oplock Break 流程

```
Client A (持有 Oplock)          Server           Client B (请求冲突访问)
─────────────────              ──────           ────────────────────
                               │<─── CREATE (写访问) ────────│
                               │
─── OPLOCK_BREAK_NOTIFICATION ─│                              │
    (Oplock → Level2/None)     │                              │
                               │                              │
─── OPLOCK_BREAK_ACK ─────────>│                              │
                               │─── CREATE Response ────────>│
```

## 9. 加密与签名设计

### 9.1 SMB3 加密

```
SMB3 加密消息流:

应用层 SMB2 消息
    │
    ▼
AES-128-CCM/GCM 加密
    │
    ├── Nonce: 11字节 (AES-CCM) 或 12字节 (AES-GCM)
    ├── AAD: Transform Header (52B) 签名字段清零
    ├── Key: 派生的 Encryption/Decryption Key
    │
    ▼
SMB2 Transform Header (0xFD00):
    ├── ProtocolId: 0xFD, 'S', 'M', 'B'
    ├── Signature: 16字节 (AES-CCM) 或 16字节 (AES-GCM)
    ├── Nonce: 11/12字节
    ├── OriginalMessageSize: 4字节
    ├── EncryptionAlgorithm: AES-128-CCM (0x0001) / AES-GCM (0x0002)
    ├── SessionId: 8字节
    │
    ▼
加密数据 + 签名
```

### 9.2 签名算法

| 方言 | 签名算法 |
|------|---------|
| SMB 2.0.2 | HMAC-SHA256 (截取前16字节) |
| SMB 2.1 | HMAC-SHA256 (截取前16字节) |
| SMB 3.0 | AES-128-CMAC |
| SMB 3.0.2 | AES-128-CMAC |
| SMB 3.1.1 | AES-128-CMAC |

## 10. 命名管道与 RPC 设计

### 10.1 IPC$ 共享

SMB 通过 IPC$ 共享提供命名管道访问，用于远程管理（SRVSVC, WKSSVC 等）。

```
SmbPipeManager:
  ├─ 内置管道:
  │   ├── \pipe\srvsvc      (Server Service - 共享枚举)
  │   ├── \pipe\wkssvc      (Workstation Service)
  │   ├── \pipe\samr        (Security Account Manager)
  │   ├── \pipe\lsarpc      (Local Security Authority)
  │   ├── \pipe\netlogon    (Net Logon)
  │   ├── \pipe\svcctl      (Service Control Manager)
  │   ├── \pipe\eventlog    (Event Log)
  │   └── \pipe\winreg      (Windows Registry)
  │
  ├─ 自定义管道:
  │   └── 通过 SmbHandler::on_pipe_open 注册
  │
  └─ 管道操作:
      ├── open: 创建/连接管道实例
      ├── read: 读取管道数据
      ├── write: 写入管道数据
      └── close: 关闭管道实例
```

### 10.2 RPC over SMB

命名管道承载 DCE/RPC 协议：
- 绑定（Bind）：客户端声明要调用的接口 UUID
- 请求（Request）：调用远程方法
- 响应（Response）：返回结果

本实现提供管道传输层，RPC 解析作为可选层。

## 11. DFS 设计

```
SmbDfsResolver:
  ├─ DFS 命名空间:
  │   \\server\root\path → \\target_server\share\path
  │
  ├─ DFS Referral 响应:
  │   版本 1-4 (V4 支持 site-cost 排序)
  │
  └─ 集成点:
      ├── TREE_CONNECT: 检测 DFS 路径
      ├── IOCTL: FSCTL_DFS_GET_REFERRALS
      └── SmbHandler::on_dfs_resolve 自定义解析
```

## 12. 复合请求（Compound）设计

SMB2 支持在一个 TCP 消息中发送多个命令（链式请求），减少 RTT：

```
SmbDispatcher::dispatch_compound():
  ├── 解析 NextCommand 链
  ├── 收集所有 (Header, Payload) 对
  ├── 依次 dispatch 每个命令
  │   ├── 相关命令共享 session_id / tree_id
  │   ├── CREATE 返回的 FileId 可被后续 READ 使用
  │   └── 中间失败通过 Status 传播
  ├── 组装复合响应
  └── 设置响应的 NextCommand 偏移
```

## 13. 多信用（Credits）机制

```
Credit 管理:
  ├── 客户端发送请求时消耗 credits (credit_charge)
  ├── 客户端请求额外 credits (credit_request)
  ├── 服务器根据负载授予 credits (credit_granted)
  ├── 每个 credit 允许发送 64KB 数据
  ├── 初始授予: 1 credit (SMB 2.0.2) 或 128+ (SMB 2.1+)
  └── 最大授予: config_.max_credits

SmbSession 中的 credit 管理:
  credits_available_ += response.credit_granted
  credits_available_ -= request.credit_charge
```

## 14. 异步通知设计

### 14.1 Change Notify

```
SmbSession → SmbChangeNotifier:
  ├── 注册目录监听 (inotify / ReadDirectoryChangesW / kqueue)
  ├── 收到变更事件
  ├── 发送 SMB2 CHANGE_NOTIFY 响应 (异步完成)
  └── 取消监听 (CANCEL 命令)
```

### 14.2 Oplock/Lease Break 通知

```
SmbLockManager:
  ├── 检测到锁冲突
  ├── 向持有者发送 OPLOCK_BREAK / LEASE_BREAK 通知
  │   (异步，不占用请求-响应对)
  ├── 等待 ACK (超时后强制 break)
  └── 授予请求者访问权限
```

## 15. 持久句柄（Durable Handles）设计

SMB 3.0+ 支持持久句柄，允许在连接中断后重新打开文件：

```
Durable Handle 生命周期:
  ├── CREATE 时设置 SMB2_CREATE_DURABLE_HANDLE_REQUEST
  ├── 服务器保存 {persistent_file_id, path, access, ...}
  ├── 连接断开 → 持久句柄进入"残留"状态
  ├── 客户端重连 → CREATE 时设置 SMB2_CREATE_DURABLE_HANDLE_RECONNECT
  ├── 服务器匹配残留句柄 → 恢复文件状态
  └── 超时未重连 → 释放句柄
```

## 16. 文件结构

```
protocol/smb/
├── CMakeLists.txt
├── include/
│   ├── smb.h                          # 统一头文件
│   ├── smb_config.h                   # SmbServerConfig
│   ├── smb_handler.h                  # SmbHandler 回调接口
│   ├── smb_server.h                   # SmbServer 核心服务器
│   ├── smb_session.h                  # SmbSession 会话
│   ├── smb_session_manager.h          # 会话管理器
│   ├── smb_dispatcher.h               # 命令分发器
│   ├── smb_share.h                    # SmbShare + SmbShareManager
│   ├── smb_file_system.h              # SmbFileSystem 接口 + LocalFileSystem
│   ├── smb_lock_manager.h             # 锁/Oplock/Lease 管理
│   ├── smb_pipe_manager.h             # 命名管道管理
│   ├── smb_dfs_resolver.h             # DFS 解析
│   ├── smb_change_notifier.h          # 变更通知
│   │
│   ├── protocol/
│   │   ├── smb2_constants.h           # SMB2 协议常量、枚举
│   │   ├── smb2_structures.h          # SMB2 协议结构体
│   │   ├── smb2_codec.h              # SMB2 编解码器
│   │   ├── smb1_negotiate.h           # SMB1 兼容协商
│   │   └── smb_netbios.h             # NetBIOS 会话服务
│   │
│   ├── auth/
│   │   ├── smb_auth.h                # 认证接口
│   │   ├── smb_ntlm.h                # NTLMv2 认证
│   │   └── smb_spnego.h              # SPNEGO 封装
│   │
│   └── crypto/
│       ├── smb_crypto.h              # 加密/签名接口
│       ├── smb_crypto_openssl.h      # OpenSSL 实现
│       └── smb_key_derivation.h      # 密钥派生
│
└── src/
    ├── smb_server.cpp                # 服务器实现
    ├── smb_session.cpp               # 会话实现
    ├── smb_session_manager.cpp       # 会话管理器实现
    ├── smb_dispatcher.cpp            # 分发器实现
    ├── smb_share.cpp                 # 共享管理实现
    ├── smb_file_system.cpp           # 本地文件系统实现
    ├── smb_lock_manager.cpp          # 锁管理实现
    ├── smb_pipe_manager.cpp          # 管道管理实现
    ├── smb_dfs_resolver.cpp          # DFS 解析实现
    ├── smb_change_notifier.cpp       # 变更通知实现
    │
    ├── protocol/
    │   ├── smb2_codec.cpp            # SMB2 编解码实现
    │   ├── smb1_negotiate.cpp        # SMB1 兼容协商实现
    │   └── smb_netbios.cpp           # NetBIOS 实现实现
    │
    ├── auth/
    │   ├── smb_ntlm.cpp              # NTLMv2 实现
    │   └── smb_spnego.cpp            # SPNEGO 实现
    │
    └── crypto/
        ├── smb_crypto_openssl.cpp    # OpenSSL 加密实现
        └── smb_key_derivation.cpp    # 密钥派生实现

server/services/
├── include/
│   └── smb_service.h                 # SmbService 服务包装
└── src/
    └── smb_service.cpp               # 服务包装实现
```

## 17. 核心流程伪代码

### 17.1 服务器主循环

```cpp
coroutine::Task<void> SmbServer::handle_connection(AsyncConnectionContext ctx) {
    SmbSession session(ctx, this);
    
    while (session.state() != SmbSession::State::closed) {
        auto read_result = co_await ctx.read_async();
        if (read_result.status != IoStatus::success) {
            break;
        }
        
        // 1. 剥离 NetBIOS 会话头
        auto nb_header = SmbNetbios::decode(read_result.data);
        
        // 2. 判断是否加密消息
        if (Smb2Codec::is_transform_header(read_result.data)) {
            // 解密
            auto decrypted = crypto_->decrypt(session, read_result.data);
            process_message(session, decrypted);
        } else {
            process_message(session, read_result.data);
        }
    }
    
    session_mgr_.remove_session(session.session_id());
}

void SmbServer::process_message(SmbSession& session, ByteBuffer& buf) {
    // 3. 解析 SMB2 Header
    auto header = Smb2Codec::decode_header(buf);
    
    // 4. 验证签名
    if (header.flags & SMB2_FLAGS_SIGNED) {
        if (!crypto_->verify_signature(header, buf, session.signing_key())) {
            send_error(session, header, STATUS_ACCESS_DENIED);
            return;
        }
    }
    
    // 5. 处理复合请求
    if (header.next_command != 0) {
        dispatcher_.dispatch_compound(session, buf);
    } else {
        dispatcher_.dispatch(session, header, buf);
    }
}
```

### 17.2 命令分发

```cpp
ByteBuffer SmbDispatcher::dispatch(SmbSession& session, const Smb2Header& header, ByteBuffer& buf) {
    switch (header.command) {
    case SMB2_NEGOTIATE:
        return handle_negotiate(session, header, buf);
    case SMB2_SESSION_SETUP:
        return handle_session_setup(session, header, buf);
    case SMB2_LOGOFF:
        return handle_logoff(session, header, buf);
    case SMB2_TREE_CONNECT:
        return handle_tree_connect(session, header, buf);
    case SMB2_TREE_DISCONNECT:
        return handle_tree_disconnect(session, header, buf);
    case SMB2_CREATE:
        return handle_create(session, header, buf);
    case SMB2_CLOSE:
        return handle_close(session, header, buf);
    case SMB2_READ:
        return handle_read(session, header, buf);
    case SMB2_WRITE:
        return handle_write(session, header, buf);
    case SMB2_QUERY_DIRECTORY:
        return handle_query_directory(session, header, buf);
    case SMB2_QUERY_INFO:
        return handle_query_info(session, header, buf);
    case SMB2_SET_INFO:
        return handle_set_info(session, header, buf);
    case SMB2_LOCK:
        return handle_lock(session, header, buf);
    case SMB2_IOCTL:
        return handle_ioctl(session, header, buf);
    case SMB2_ECHO:
        return handle_echo(session, header, buf);
    case SMB2_CHANGE_NOTIFY:
        return handle_change_notify(session, header, buf);
    case SMB2_FLUSH:
        return handle_flush(session, header, buf);
    case SMB2_CANCEL:
        return handle_cancel(session, header, buf);
    case SMB2_OPLOCK_BREAK:
        return handle_oplock_break(session, header, buf);
    default:
        return Smb2Codec::build_error_response(header, STATUS_NOT_IMPLEMENTED);
    }
}
```

### 17.3 CREATE 命令处理

```cpp
ByteBuffer SmbDispatcher::handle_create(SmbSession& session, const Smb2Header& header, ByteBuffer& buf) {
    auto request = Smb2Codec::decode_create_request(buf);
    
    // 1. 查找 TreeConnection
    auto* tree = session.find_tree(header.tree_id);
    if (!tree) return error(header, STATUS_NETWORK_NAME_DELETED);
    
    // 2. 解析路径
    std::string full_path = tree->share()->resolve_path(request.buffer);
    
    // 3. 处理 Create Contexts (durable handle, lease, etc.)
    CreateContextResult ctx_result = parse_create_contexts(request.create_contexts);
    
    // 4. 调用文件系统
    auto open_result = tree->file_system()->open(
        full_path, request.desired_access, request.create_disposition, request.create_options);
    
    if (!open_result.success) {
        return error(header, open_result.status);
    }
    
    // 5. 分配 FileId
    auto file_id = session.allocate_file_id();
    
    // 6. 创建 OpenFile 并注册
    OpenFile file;
    file.file_id = file_id;
    file.persistent_id = file_id.persistent;
    file.path = full_path;
    file.access_mask = request.desired_access;
    file.is_directory = open_result.is_directory;
    file.oplock_level = ctx_result.oplock_level;
    file.lease_key = ctx_result.lease_key;
    file.lease_state = ctx_result.lease_state;
    file.file_handle = open_result.handle;
    file.durable = ctx_result.durable;
    tree->add_open_file(file_id, file);
    
    // 7. 请求 Oplock/Lease
    if (ctx_result.oplock_level != SMB2_OPLOCK_LEVEL_NONE) {
        lock_mgr_.request_oplock(file_id, ctx_result.oplock_level);
    }
    if (ctx_result.lease_state != 0) {
        lock_mgr_.request_lease(ctx_result.lease_key, ctx_result.lease_state);
    }
    
    // 8. 构建 Create Response
    return Smb2Codec::encode_create_response(header, file, open_result, ctx_result);
}
```

## 18. 构建系统集成

### 18.1 新增 CMake 目标

- `SmbProto`：SMB 协议库，链接 `Core` + OpenSSL

### 18.2 修改的 CMakeLists.txt

| 文件 | 变更 |
|------|------|
| `CMakeLists.txt` | 新增 `add_subdirectory(protocol/smb)` |
| `protocol/smb/CMakeLists.txt` | 新建，定义 `SmbProto` 库 |
| `server/services/CMakeLists.txt` | `target_link_libraries` 新增 `SmbProto` |

## 19. 服务注册

```cpp
#include "smb_service.h"

yuan::net::smb::SmbServerConfig smb_config;
smb_config.server_name = "YUAN-SMB";
smb_config.enable_encryption = true;
smb_config.require_signing = true;

// 添加文件共享
smb_config.shares.push_back({
    .name = "public",
    .comment = "Public Share",
    .type = yuan::net::smb::ShareType::disk,
    .path = "/srv/smb/public"
});

// 添加 IPC$ (自动)
smb_config.shares.push_back({
    .name = "IPC$",
    .comment = "Remote IPC",
    .type = yuan::net::smb::ShareType::pipe
});

application.add_typed_service<yuan::server::SmbService>(
    "smb",
    std::make_shared<yuan::server::SmbService>(445, smb_config),
    "server.smb",
    1);
```

## 20. 性能优化设计

| 优化点 | 方案 |
|--------|------|
| 零拷贝传输 | READ 响应使用 `pread64()` 直接写入 ByteBuffer，避免中间拷贝 |
| 多信用 | SMB 2.1+ 支持一次授予多个 credit，减少 RTT |
| 复合请求 | CREATE+READ, CREATE+WRITE 在一个 RTT 完成 |
| 大 MTU | SMB 2.1+ 支持 1MB 消息（需多 credit） |
| 异步 IO | 所有文件操作在协程中执行，不阻塞 EventLoop |
| Oplock/Lease | 减少网络 IO，客户端可缓存数据 |
| 连接复用 | 单 TCP 连接多路复用（多 Session） |
| ByteBuffer 池 | 预分配大块 buffer，减少内存分配开销 |
| AES-NI | 利用 OpenSSL 的 AES 硬件加速 |

## 21. 安全考虑

| 安全特性 | 方案 |
|---------|------|
| 签名 | 防止消息篡改，SMB3 使用 AES-CMAC |
| 加密 | SMB3 全链路加密，AES-128-CCM/GCM |
| 预认证完整性 | SMB 3.1.1 在协商阶段验证握手完整性 |
| 认证 | NTLMv2 + SPNEGO，可扩展 Kerberos |
| 访问控制 | SmbHandler 回调实现 ACL 检查 |
| 路径遍历 | 严格验证路径，防止 `..` 越界 |
| 拒绝服务 | 限制 session 数、credit 数、消息大小 |

## 22. 与现有协议对比

| 特性 | HttpServer | FtpServer | Socks5Server | SmbServer |
|------|-----------|-----------|-------------|-----------|
| 端口 | 80/443 | 21 | 1080 | 445 |
| 传输 | TCP | TCP+TCP/数据 | TCP+UDP | TCP |
| 认证 | Basic/Bearer | USER/PASS | Username/Password | NTLMv2/SPNEGO |
| 加密 | TLS | TLS | 无 | AES-CCM/GCM |
| 签名 | 无 | 无 | 无 | AES-CMAC/HMAC-SHA256 |
| 会话 | Cookie | 登录态 | 无状态 | Session ID + Tree ID |
| 文件操作 | GET/PUT | RETR/STOR | 无 | CREATE/READ/WRITE/CLOSE/LOCK |
| 锁 | 无 | 无 | 无 | Byte-Range Lock + Oplock/Lease |
| 复合请求 | Pipeline | 无 | 无 | Compound Chain |
| 异步通知 | SSE | 无 | 无 | Change Notify / Oplock Break |
| 命名管道 | 无 | 无 | 无 | IPC$ |
| 状态复杂度 | 中 | 高 | 低 | 最高 |

## 23. 实现优先级与里程碑

### Phase 1 - 核心协议（MVP）
- [x] SMB2 协议编解码（Header + 所有命令结构体）
- [x] NetBIOS 会话服务
- [x] NEGOTIATE 命令（SMB1 兼容协商）
- [x] SESSION_SETUP 命令（NTLMv2 认证）
- [x] TREE_CONNECT / TREE_DISCONNECT
- [x] CREATE / CLOSE
- [x] READ / WRITE
- [x] ECHO
- [x] LOGOFF
- [x] 本地文件系统实现
- [x] SmbServer + SmbService 集成

### Phase 2 - 文件系统完整性
- [x] QUERY_DIRECTORY
- [x] QUERY_INFO / SET_INFO
- [x] FLUSH
- [x] IOCTL（FSCTL 基础）
- [x] 共享管理（SmbShareManager）

### Phase 3 - 锁与缓存
- [x] LOCK 命令（Byte-Range Lock）
- [x] Oplock（SMB 2.1）
- [x] Lease（SMB 3.x）
- [x] OPLOCK_BREAK 通知

### Phase 4 - 安全与加密
- [x] SMB3 签名（AES-128-CMAC）
- [x] SMB3 加密（AES-128-CCM/GCM）
- [x] 预认证完整性（SMB 3.1.1）
- [x] 密钥派生（SP800-108 / KDF）

### Phase 5 - 高级特性
- [x] 复合请求（Compound）
- [x] 多信用（Credits）
- [x] CANCEL 命令
- [x] CHANGE_NOTIFY
- [x] 持久句柄（Durable Handle）

### Phase 6 - 企业特性
- [x] 命名管道（IPC$）
- [x] DFS 解析
- [x] SPNEGO 封装
- [x] 管道 RPC 框架

## 24. 测试策略

### 24.1 单元测试

- Smb2Codec 编解码往返验证
- SmbNetbios 帧解析
- NTLMv2 哈希计算验证
- SPNEGO token 编解码
- AES-CCM/GCM 加解密验证
- 密钥派生验证
- 锁兼容性矩阵检查
- Credit 管理逻辑
- 复合请求解析

### 24.2 集成测试

- 完整协商 + 认证流程
- Tree Connect + 文件操作
- 目录遍历
- 锁冲突与 break
- 加密会话端到端

### 24.3 互操作性测试

```bash
# Linux smbclient
smbclient //127.0.0.1/public -U test%password

# mount.cifs
mount -t cifs //127.0.0.1/public /mnt/smb -o username=test,password=password

# Windows
net use Z: \\127.0.0.1\public /user:test password
```
