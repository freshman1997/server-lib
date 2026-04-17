# SSH 测试矩阵

本文档定义 SSH-2.0 协议实现的完整测试矩阵，覆盖传输层、认证层、连接层、SFTP 子系统和端口转发。

---

## 1. 传输层（Transport Layer）— Phase 1

### 1.1 版本交换

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T1.1.1 | 正常版本交换 | `SSH-2.0-OpenSSH_8.9\r\n` | 发送 `SSH-2.0-YuanSSH_1.0\r\n`，状态转为 version_exchanged | P0 |
| T1.1.2 | 忽略 banner 行 | 多行 banner + `SSH-2.0-Client\r\n` | 跳过 banner，正确解析版本行 | P0 |
| T1.1.3 | 拒绝 SSH-1.x | `SSH-1.99-Client\r\n` | 发送 DISCONNECT (protocol_version_not_supported) | P0 |
| T1.1.4 | 拒绝无效格式 | `INVALID\r\n` | 发送 DISCONNECT | P1 |
| T1.1.5 | 版本行超长 | >253 字节的版本行 | 发送 DISCONNECT | P1 |
| T1.1.6 | 超时无版本行 | 连接后不发送数据 | 超时后断开连接 | P2 |

### 1.2 KEXINIT 协商

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T1.2.1 | curve25519-sha256 协商 | 客户端支持 curve25519-sha256 | 协商成功，kex_name = curve25519-sha256 | P0 |
| T1.2.2 | ecdh-sha2-nistp256 协商 | 客户端仅支持 ecdh-sha2-nistp256 | 协商成功 | P0 |
| T1.2.3 | ecdh-sha2-nistp384 协商 | 客户端仅支持 ecdh-sha2-nistp384 | 协商成功 | P1 |
| T1.2.4 | ecdh-sha2-nistp521 协商 | 客户端仅支持 ecdh-sha2-nistp521 | 协商成功 | P1 |
| T1.2.5 | diffie-hellman-group14-sha256 | 客户端仅支持 group14-sha256 | 协商成功 | P0 |
| T1.2.6 | diffie-hellman-group16-sha512 | 客户端仅支持 group16-sha512 | 协商成功 | P1 |
| T1.2.7 | diffie-hellman-group18-sha512 | 客户端仅支持 group18-sha512 | 协商成功 | P1 |
| T1.2.8 | 无共同算法 | 客户端算法列表与服务端无交集 | 发送 DISCONNECT (key_exchange_failed) | P0 |
| T1.2.9 | host_key_algorithms 正确协商 | 客户端支持 ssh-ed25519 和 rsa-sha2-256 | 选择服务端首选的主机密钥算法 | P0 |
| T1.2.10 | first_kex_packet_follows | 客户端设置 first_kex_packet_follows=true | 正确处理可能的猜测包 | P2 |

### 1.3 密钥交换

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T1.3.1 | curve25519 KEX 完成 | 完整 curve25519 交换 | 双方计算相同的共享密钥 H, K | P0 |
| T1.3.2 | ECDH-NIST KEX 完成 | 完整 ECDH-NIST 交换 | 双方计算相同的共享密钥 | P0 |
| T1.3.3 | DH-Group14 KEX 完成 | 完整 DH-Group14 交换 | 双方计算相同的共享密钥 | P0 |
| T1.3.4 | DH-GEX 协商 | DH-GEX 请求 min/preferred/max | 返回合适的素数/生成元 | P1 |
| T1.3.5 | KEX 哈希使用正确算法 | curve25519 → sha256, group18 → sha512 | kex_hash_name 正确设置 | P0 |
| T1.3.6 | exchange_hash 正确性 | 已知测试向量 | hash 值与参考实现一致 | P0 |

### 1.4 密钥派生

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T1.4.1 | derive_key 使用正确 hash | kex_hash_name=sha256 | 使用 SHA-256 派生 | P0 |
| T1.4.2 | derive_key 使用 sha512 | kex_hash_name=sha512 (group18) | 使用 SHA-512 派生 | P0 |
| T1.4.3 | derive_key 使用 sha1 | kex_hash_name=sha1 (group1 兼容) | 使用 SHA-1 派生 | P1 |
| T1.4.4 | 6 个密钥派生 | K, H, session_id | A-E + initial IV 正确派生 | P0 |
| T1.4.5 | 长密钥串联扩展 | 需要 >hash_output_size 的密钥 | 正确串联多轮 hash | P1 |

### 1.5 NEWKEYS 切换

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T1.5.1 | 收到 NEWKEYS 后激活加密 | NEWKEYS 消息 | cipher_context 激活，后续包加密 | P0 |
| T1.5.2 | 发送 NEWKEYS 后加密出站 | 发送 NEWKEYS | 后续出站报文加密 | P0 |
| T1.5.3 | NEWKEYS 前收到加密包 | 加密包在 NEWKEYS 前 | 丢弃/断开 | P0 |

### 1.6 加密算法

| ID | 测试项 | 算法 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T1.6.1 | AES-128-GCM 加解密 | aes128-gcm@openssh.com | 正确加解密 + AEAD tag 验证 | P0 |
| T1.6.2 | AES-256-GCM 加解密 | aes256-gcm@openssh.com | 正确加解密 + AEAD tag 验证 | P0 |
| T1.6.3 | AES-GCM nonce 递增 | 连续加密多个包 | nonce = IV XOR seq_number | P0 |
| T1.6.4 | AES-128-CTR 加解密 | aes128-ctr | 正确加解密 + separate MAC | P0 |
| T1.6.5 | AES-256-CTR 加解密 | aes256-ctr | 正确加解密 + separate MAC | P0 |
| T1.6.6 | ChaCha20-Poly1305 | chacha20-poly1305@openssh.com | 正确加解密 + AEAD | P0 |
| T1.6.7 | AEAD encrypt/decrypt 委托 | AEAD cipher 调用 encrypt() | 委托到 encrypt_aead()，返回含 tag 的数据 | P0 |
| T1.6.8 | 解密 tag 验证失败 | 篡改密文 | 返回空/错误 | P0 |

### 1.7 MAC 算法

| ID | 测试项 | 算法 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T1.7.1 | HMAC-SHA2-256 | hmac-sha2-256 | MAC 验证通过 | P0 |
| T1.7.2 | HMAC-SHA2-512 | hmac-sha2-512 | MAC 验证通过 | P0 |
| T1.7.3 | HMAC-SHA1 | hmac-sha1 | MAC 验证通过 | P1 |

### 1.8 Rekey

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T1.8.1 | 主动 rekey | 数据量阈值触发 | KEXINIT 发起，rekey 完成 | P1 |
| T1.8.2 | 被动 rekey | 收到对端 KEXINIT | 正确处理，rekey 完成 | P1 |
| T1.8.3 | Rekey 期间数据排队 | rekey 期间收到数据报文 | 正确排队，rekey 后处理 | P2 |

---

## 2. 认证层（Authentication Layer）— Phase 2

### 2.1 认证状态机

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T2.1.1 | SERVICE_REQUEST ssh-userauth | 有效的 service request | 状态转为 service_requested，发送 SERVICE_ACCEPT | P0 |
| T2.1.2 | SERVICE_REQUEST 无效服务 | "ssh-connection" 在认证前 | 返回 FAILURE | P0 |
| T2.1.3 | USERAUTH_REQUEST 有效密码 | 正确的用户名+密码 | 状态转为 authenticated，返回 SUCCESS | P0 |
| T2.1.4 | 认证失败计数 | 连续 6 次失败 | 第 6 次后状态转为 failed，连接断开 | P0 |
| T2.1.5 | 用户名一致性 | 第二次请求不同用户名 | 返回 FAILURE | P1 |
| T2.1.6 | partial_success | 部分成功多步认证 | 返回 FAILURE + 允许的方法列表 | P2 |
| T2.1.7 | allowed_methods_string | 注册 password + publickey | 返回 "password,publickey" | P0 |

### 2.2 密码认证

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T2.2.1 | Handler 接受密码 | handler 返回 SUCCESS | 认证成功 | P0 |
| T2.2.2 | Handler 拒绝密码 | handler 返回 FAILURE | 认证失败 | P0 |
| T2.2.3 | 方法回退（无 handler） | handler 默认，非空密码 | SshAuthPassword::authenticate() 回退，返回 SUCCESS | P0 |
| T2.2.4 | 方法回退空密码 | handler 默认，空密码 | 返回 FAILURE | P0 |
| T2.2.5 | 凭证解析 | has_sig=false + 密码字符串 | 正确解析密码字段 | P0 |
| T2.2.6 | 截断数据 | 密码长度字段 > 剩余数据 | 返回 FAILURE | P1 |

### 2.3 公钥认证

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T2.3.1 | 查询阶段（无签名） | has_signature=false | 返回 NEED_MORE + PK_OK | P0 |
| T2.3.2 | 签名验证成功 | 正确签名 | 返回 SUCCESS | P0 |
| T2.3.3 | 签名验证失败 | 错误签名 | 返回 FAILURE | P0 |
| T2.3.4 | 方法回退验证签名 | handler 默认，正确签名 | authenticate() 调用 verify_signature() | P0 |
| T2.3.5 | 签名内容正确性 | session_id + auth 请求数据 | 签名内容按 RFC 4252 Section 7 构造 | P0 |
| T2.3.6 | 算法名解析 | ssh-ed25519 / ecdsa-sha2-nistp256 / rsa-sha2-256 | 正确分派到对应验证函数 | P0 |

### 2.4 键盘交互认证

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T2.4.1 | 初始请求 | keyboard-interactive 请求 | 返回 NEED_MORE + INFO_REQUEST | P0 |
| T2.4.2 | 挑战构建 | build_challenge() | 返回包含 "Password: " 提示的 INFO_REQUEST | P0 |
| T2.4.3 | 响应验证成功 | 非空响应 | 返回 SUCCESS | P0 |
| T2.4.4 | 响应验证失败 | 空响应 | 返回 FAILURE | P0 |
| T2.4.5 | 多轮交互 | handler 返回 NEED_MORE | 发送新的 INFO_REQUEST | P1 |
| T2.4.6 | needs_more() 返回 true | 调用 needs_more() | 返回 true | P0 |
| T2.4.7 | process_response 回退 | handler 默认，非空响应 | process_response() 回退，返回 SUCCESS | P0 |

---

## 3. 连接层（Connection Layer）— Phase 3

### 3.1 通道管理

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T3.1.1 | 打开 session 通道 | CHANNEL_OPEN "session" | CHANNEL_OPEN_CONFIRMATION | P0 |
| T3.1.2 | 通道数限制 | 超过 max_channels | CHANNEL_OPEN_FAILURE (resource_shortage) | P0 |
| T3.1.3 | 未知通道类型 | CHANNEL_OPEN "unknown" | CHANNEL_OPEN_FAILURE (unknown_channel_type) | P0 |
| T3.1.4 | 通道关闭 | CHANNEL_CLOSE | 通道状态转为 closed，清理资源 | P0 |
| T3.1.5 | 通道 EOF | CHANNEL_EOF | 通道状态转为 eof | P0 |

### 3.2 窗口流控

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T3.2.1 | 正常数据传输 | 数据 < 窗口大小 | CHANNEL_DATA 正常发送 | P0 |
| T3.2.2 | 窗口调整 | WINDOW_ADJUST | 本地窗口增加 | P0 |
| T3.2.3 | 自动窗口调整 | 窗口低于阈值 | 自动发送 WINDOW_ADJUST | P0 |
| T3.2.4 | 窗口耗尽 | 数据 > 窗口大小 | 数据缓冲，等待窗口调整 | P0 |
| T3.2.5 | 超大数据包 | 数据 > max_packet_size | 丢弃/返回空 | P1 |

### 3.3 通道请求

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T3.3.1 | exec 请求 | "echo hello" | handler->on_exec_request() 调用，返回 SUCCESS/FAILURE | P0 |
| T3.3.2 | subsystem 请求 | "sftp" | handler->on_subsystem_request() 调用，创建 SftpHandler | P0 |
| T3.3.3 | env 请求 | name="FOO", value="bar" | handler->on_env_request() 调用 | P1 |
| T3.3.4 | pty-req 请求 | PTY 参数 | handler->on_pty_request() 调用，默认返回 false | P1 |
| T3.3.5 | shell 请求 | 空 | handler->on_shell_request() 调用，默认返回 false | P1 |
| T3.3.6 | signal 请求 | "TERM" | handler->on_signal() 调用 | P2 |
| T3.3.7 | window-change | 新尺寸 | handler->on_window_change() 调用 | P2 |
| T3.3.8 | want_reply=false | 请求不需要回复 | 不发送 SUCCESS/FAILURE | P0 |
| T3.3.9 | want_reply=true | 请求需要回复 | 发送 SUCCESS 或 FAILURE | P0 |

### 3.4 全局请求

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T3.4.1 | tcpip-forward | 绑定地址+端口 | handler->on_tcpip_forward() 调用，返回分配端口 | P0 |
| T3.4.2 | cancel-tcpip-forward | 绑定地址+端口 | handler->on_cancel_tcpip_forward() 调用 | P0 |
| T3.4.3 | 未知全局请求 | 未知请求名 | REQUEST_FAILURE | P1 |

---

## 4. SFTP 子系统 — Phase 4

### 4.1 协议初始化

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T4.1.1 | SSH_FXP_INIT | version=3 | SSH_FXP_VERSION version=3 | P0 |
| T4.1.2 | 不支持的版本 | version=6 | 回复 version=3（降级） | P1 |

### 4.2 文件操作

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T4.2.1 | 打开文件读 | SSH_FXF_READ | 返回文件句柄 | P0 |
| T4.2.2 | 打开文件写 | SSH_FXF_WRITE + SSH_FXF_CREAT | 返回文件句柄 | P0 |
| T4.2.3 | 打开文件追加 | SSH_FXF_WRITE + SSH_FXF_APPEND | 返回文件句柄 | P0 |
| T4.2.4 | 打开文件独占 | SSH_FXF_CREAT + SSH_FXF_EXCL | 文件已存在时返回 SSH_FX_FILE_ALREADY_EXISTS | P1 |
| T4.2.5 | 读取文件 | offset=0, len=1024 | 返回文件数据 | P0 |
| T4.2.6 | 读取 EOF | offset=文件末尾 | 返回 SSH_FX_EOF | P0 |
| T4.2.7 | 写入文件 | offset=0, data | 写入成功 | P0 |
| T4.2.8 | 关闭文件 | 有效句柄 | SSH_FX_OK | P0 |
| T4.2.9 | 无效句柄 | 不存在的句柄 | SSH_FX_INVALID_HANDLE | P0 |
| T4.2.10 | 大文件读取 | len > SFTP_MAX_READ_SIZE | 截断到最大值 | P1 |

### 4.3 目录操作

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T4.3.1 | 打开目录 | 有效路径 | 返回目录句柄 | P0 |
| T4.3.2 | 读取目录 | 有效句柄 | 返回目录项列表 | P0 |
| T4.3.3 | 读取目录 EOF | 目录末尾 | 返回 SSH_FX_EOF | P0 |
| T4.3.4 | 创建目录 | 路径+属性 | SSH_FX_OK | P0 |
| T4.3.5 | 删除空目录 | 空目录路径 | SSH_FX_OK | P0 |
| T4.3.6 | 删除非空目录 | 非空目录路径 | SSH_FX_DIR_NOT_EMPTY | P0 |

### 4.4 属性操作

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T4.4.1 | stat | 有效路径 | 返回文件属性（跟随符号链接） | P0 |
| T4.4.2 | lstat | 有效路径 | 返回文件属性（不跟随符号链接） | P0 |
| T4.4.3 | fstat | 有效句柄 | 返回文件属性 | P0 |
| T4.4.4 | setstat - 截断 | size=0 | 文件截断成功 | P1 |
| T4.4.5 | setstat - 权限 | permissions=0644 | chmod 成功 | P1 |
| T4.4.6 | setstat - 时间 | atime/mtime | utimensat 成功 | P1 |
| T4.4.7 | fsetstat | 有效句柄+属性 | 属性修改成功 | P1 |

### 4.5 路径操作

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T4.5.1 | realpath | 相对路径 | 返回绝对路径 | P0 |
| T4.5.2 | rename 无覆盖 | 目标不存在 | 重命名成功 | P0 |
| T4.5.3 | rename 覆盖 | SSH_FXP_RENAME_OVERWRITE + 目标已存在 | 重命名成功 | P0 |
| T4.5.4 | rename 拒绝覆盖 | 无 OVERWRITE 标志 + 目标已存在 | SSH_FX_FILE_ALREADY_EXISTS | P0 |
| T4.5.5 | rename 覆盖目录 | OVERWRITE + 目标是目录 | SSH_FX_FAILURE | P1 |
| T4.5.6 | readlink | 符号链接路径 | 返回链接目标 | P0 |
| T4.5.7 | symlink | 链接路径+目标路径 | 符号链接创建成功 | P0 |
| T4.5.8 | symlink 绝对目标 | 绝对路径目标 | 目标路径通过 resolve_path 验证 | P0 |
| T4.5.9 | remove | 文件路径 | 删除成功 | P0 |

### 4.6 安全性

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T4.6.1 | 路径遍历防护 | `/../../../etc/passwd` | resolve_path 返回空 | P0 |
| T4.6.2 | 越界访问 | 路径在 root_dir 之外 | resolve_path 返回空 | P0 |
| T4.6.3 | 符号链接越界 | 指向 root_dir 外的符号链接 | realpath 检测，拒绝 | P0 |
| T4.6.4 | 句柄资源释放 | 析构 SshLocalFileSystem | 所有文件/目录句柄关闭 | P0 |

---

## 5. 端口转发 — Phase 5

### 5.1 本地端口转发（direct-tcpip）

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T5.1.1 | direct-tcpip 通道打开 | 有效 host+port | handler->on_direct_tcpip() 调用 | P0 |
| T5.1.2 | handler 拒绝转发 | handler 返回 false | CHANNEL_OPEN_FAILURE (administratively_prohibited) | P0 |
| T5.1.3 | 无效参数 | port=0 | CHANNEL_OPEN_FAILURE (connect_failed) | P0 |
| T5.1.4 | 数据中继 | 发送数据到通道 | 数据转发到目标连接 | P0 |
| T5.1.5 | 目标连接失败 | 无法连接目标 | 通道关闭 | P1 |
| T5.1.6 | handle_direct_tcpip 返回 | 有效消息 | 返回解析后的 ByteBuffer（host+port+orig_addr+orig_port） | P0 |

### 5.2 远程端口转发（tcpip-forward）

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T5.2.1 | 注册远程转发 | bind_addr+bind_port | 成功注册，返回分配端口 | P0 |
| T5.2.2 | 重复注册 | 已存在的绑定 | 返回 false | P0 |
| T5.2.3 | 取消远程转发 | 已注册的绑定 | 成功取消 | P0 |
| T5.2.4 | 取消不存在的绑定 | 未注册的绑定 | 返回 false | P1 |

---

## 6. 主机密钥管理 — 跨层

### 6.1 密钥加载

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T6.1.1 | 加载 Ed25519 PEM 密钥 | Ed25519 私钥文件 | 密钥加载成功，crypto 指针有效 | P0 |
| T6.1.2 | 加载 ECDSA 密钥 | ECDSA 私钥文件 | 密钥加载成功 | P0 |
| T6.1.3 | 加载 RSA 密钥 | RSA 私钥文件 | 密钥加载成功 | P0 |
| T6.1.4 | 无效密钥文件 | 格式错误 | 返回 false | P0 |
| T6.1.5 | 悬空 crypto 指针 | load_key 后 | crypto_ 存储在 unique_ptr 中，不悬空 | P0 |

### 6.2 密钥编码

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T6.2.1 | Ed25519 public_key_blob | 加载的密钥 | 正确的 SSH wire 格式: string("ssh-ed25519") + raw(32 bytes) | P0 |
| T6.2.2 | Ed25519 签名 | 待签名数据 | string("ssh-ed25519") + raw(64 bytes signature) | P0 |
| T6.2.3 | Ed25519 验证 | 正确签名 | 验证通过 | P0 |
| T6.2.4 | Ed25519 验证失败 | 错误签名 | 验证失败 | P0 |
| T6.2.5 | RSA SHA-256 签名 | rsa-sha2-256 | 签名/验证正确 | P0 |
| T6.2.6 | RSA SHA-512 签名 | rsa-sha2-512 | 签名/验证正确 | P0 |
| T6.2.7 | ECDSA 签名 | ecdsa-sha2-nistp256 | 签名/验证正确 | P0 |
| T6.2.8 | fingerprint | Ed25519 公钥 | SHA256:base64 格式指纹 | P1 |

### 6.3 资源管理

| ID | 测试项 | 输入 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T6.3.1 | EVP_MD_CTX 泄漏 | 多次签名/验证 | 无内存泄漏 | P0 |
| T6.3.2 | EVP_PKEY 泄漏 | load_key_pair | 无内存泄漏 | P0 |

---

## 7. 集成测试 — Phase 6

### 7.1 OpenSSH 客户端兼容性

| ID | 测试项 | 命令 | 预期输出 | 优先级 |
|----|--------|------|----------|--------|
| T7.1.1 | 完整密码认证连接 | `ssh -o PreferredAuthentications=password user@host` | 认证成功 | P0 |
| T7.1.2 | 公钥认证连接 | `ssh -i ~/.ssh/id_ed25519 user@host` | 认证成功 | P0 |
| T7.1.3 | 执行远程命令 | `ssh user@host "echo hello"` | 输出 "hello" | P0 |
| T7.1.4 | SFTP 连接 | `sftp user@host` | 连接成功 | P0 |
| T7.1.5 | 本地端口转发 | `ssh -L 8080:target:80 user@host` | 转发工作 | P0 |
| T7.1.6 | 远程端口转发 | `ssh -R 9090:localhost:90 user@host` | 转发工作 | P0 |
| T7.1.7 | 算法协商 curve25519 | `ssh -o KexAlgorithms=curve25519-sha256` | 握手成功 | P0 |
| T7.1.8 | 算法协商 ecdh | `ssh -o KexAlgorithms=ecdh-sha2-nistp256` | 握手成功 | P0 |
| T7.1.9 | 密码认证失败 | 错误密码 | 认证失败，连接未断开（直到 max_attempts） | P0 |
| T7.1.10 | 认证次数耗尽 | 6 次失败 | 连接断开 | P0 |

### 7.2 算法组合矩阵

| KEX | Host Key | Cipher | MAC | 测试 ID |
|-----|----------|--------|-----|---------|
| curve25519-sha256 | ssh-ed25519 | aes128-gcm | (AEAD) | TM.1 |
| curve25519-sha256 | ssh-ed25519 | aes256-gcm | (AEAD) | TM.2 |
| curve25519-sha256 | ssh-ed25519 | chacha20-poly1305 | (AEAD) | TM.3 |
| curve25519-sha256 | rsa-sha2-256 | aes128-ctr | hmac-sha2-256 | TM.4 |
| curve25519-sha256 | ecdsa-sha2-nistp256 | aes256-ctr | hmac-sha2-512 | TM.5 |
| ecdh-sha2-nistp256 | ssh-ed25519 | aes128-gcm | (AEAD) | TM.6 |
| ecdh-sha2-nistp384 | ecdsa-sha2-nistp384 | aes256-gcm | (AEAD) | TM.7 |
| ecdh-sha2-nistp521 | ecdsa-sha2-nistp521 | chacha20-poly1305 | (AEAD) | TM.8 |
| diffie-hellman-group14-sha256 | ssh-ed25519 | aes128-ctr | hmac-sha2-256 | TM.9 |
| diffie-hellman-group16-sha512 | rsa-sha2-512 | aes256-gcm | (AEAD) | TM.10 |
| diffie-hellman-group18-sha512 | ssh-ed25519 | chacha20-poly1305 | (AEAD) | TM.11 |

### 7.3 安全性测试

| ID | 测试项 | 预期输出 | 优先级 |
|----|--------|----------|--------|
| T7.3.1 | 路径遍历攻击防护 | 所有 SFTP 操作被限制在 root_dir 内 | P0 |
| T7.3.2 | 协议降级攻击 | 拒绝不安全的算法 | P0 |
| T7.3.3 | 认证暴力破解防护 | max_attempts 后断开 | P0 |
| T7.3.4 | 加密包篡改 | AEAD/MAC 验证失败，断开连接 | P0 |
| T7.3.5 | 超大报文 | 超过 SSH_MAX_PACKET_SIZE 的包被拒绝 | P1 |

### 7.4 性能与稳定性

| ID | 测试项 | 预期输出 | 优先级 |
|----|--------|----------|--------|
| T7.4.1 | 并发连接 100 | 所有连接正常工作 | P0 |
| T7.4.2 | 大文件传输 >1GB | 无内存泄漏，传输正确 | P1 |
| T7.4.3 | 长时间运行 | 无内存泄漏，无文件句柄泄漏 | P1 |
| T7.4.4 | Rekey 期间端口转发不中断 | 转发数据完整 | P2 |
| T7.4.5 | 零拷贝验证 | 大数据传输时无不必要的内存拷贝 | P2 |

---

## 8. 缺陷修复回归测试

以下为已修复缺陷的回归测试用例，确保修复不退化。

| ID | 原始缺陷 | 回归测试 | 优先级 |
|----|----------|----------|--------|
| TR.1 | R5: SshHostKeyProvider 悬空 crypto 指针 | load_key 后持续使用密钥，不崩溃 | P0 |
| TR.2 | R2+R3: build_kex_init 用 cipher 列表代替 host_key 列表 | KEXINIT 中 server_host_key_algorithms 正确 | P0 |
| TR.3 | R1: derive_key 硬编码 hmac_sha256 | group18-sha512 使用 SHA-512 派生 | P0 |
| TR.4 | R7: AES-GCM nonce 不递增 | 连续加密多个包，nonce 正确 XOR | P0 |
| TR.5 | R9: EVP_MD_CTX 泄漏 | 反复签名/验证，无内存增长 | P0 |
| TR.6 | R10: resolve_path 路径遍历 | `../../etc/passwd` 被拒绝 | P0 |
| TR.7 | R11: symlink 绝对目标未验证 | 绝对路径符号链接经过 resolve_path | P0 |
| TR.8 | R12: 文件句柄泄漏 | SshLocalFileSystem 析构后无泄漏 | P0 |
| TR.9 | R6: SshAuthPublickey 无 crypto | publickey 认证能正确验证签名 | P0 |
| TR.10 | S4+S5: AEAD encrypt/decrypt 返回空 | AEAD 加密后能正确解密 | P0 |
| TR.11 | S1+S2: SshAuthPassword 始终 FAILURE | 无 handler 时非空密码认证成功 | P0 |
| TR.12 | S3: keyboard-interactive 不验证响应 | process_response 验证非空响应 | P0 |
| TR.13 | S6: handle_direct_tcpip 返回空 | 返回包含解析数据的 ByteBuffer | P0 |
| TR.14 | R13: rename 忽略 flags | 无 OVERWRITE 时已存在目标返回错误 | P0 |
| TR.15 | R8: ed25519 write_string 二进制 | public_key_blob 签名格式正确（write_raw） | P0 |

---

## 优先级说明

- **P0**: 必须通过，阻塞发布
- **P1**: 应当通过，影响功能完整性
- **P2**: 可选，影响健壮性
