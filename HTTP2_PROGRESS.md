# HTTP/2 实现进度记录

> 更新时间: 2026-04-24

## 当前阻塞：HPACK 解码仍然失败

服务器能正确完成 ALPN h2 协商、SETTINGS 交换，但收到客户端 HEADERS 帧后 HPACK 解码返回 false。

### 最新测试结果

```
[HTTP2] HEADERS frame: stream_id=1, length=22, flags=0x04
[HTTP2] HPACK decode failed, header_block size=22, hex=82 44 86 60 75 99 84 95 09 87 41 8a a0 e4 1d 13 9d 09 b8 f0 1e 07
```

Python hpack 正确解码结果：
- `:method: GET`
- `:path: /api/test`
- `:scheme: https`
- `:authority: localhost:8080`

C++ HPACK decoder 返回 false。

### HPACK 字节手动追踪

```
82 -> Indexed Header, index=2 -> :method GET ✓
44 -> Literal with Incremental Indexing, name_index=4(:path), 6-bit prefix
86 -> String: huffman=true, length=6, 7-bit prefix
60 75 99 84 95 09 -> Huffman encoded "/api/test"
87 -> Indexed Header, index=7 -> :scheme https ✓
41 -> Literal with Incremental Indexing, name_index=1(:authority), 6-bit prefix
8a -> String: huffman=true, length=10, 7-bit prefix
a0 e4 1d 13 9d 09 b8 f0 1e 07 -> Huffman encoded "localhost:8080"
```

---

## 已完成的工作

### 1. ALPN + h2 over TLS
- SSL 层添加 ALPN 协商支持
- `HttpServer::handle_connection()` 检查 ALPN 结果，若为 "h2" 则直接进入 HTTP/2 模式

### 2. HPACK Encoder 完整实现
- 完整静态表，通用 `encode_header()` 自动选择最佳编码
- 动态表 + 增量索引
- Huffman 编码（`huffman_encode()` 带 bit-packing）

### 3. Session 增强
- `send_headers()`, `send_data()` (带帧分片), `send_rst_stream()`, `send_goaway()`
- `GoawayBridge` 回调, `connection_send_window_` 追踪, 流状态访问器

### 4. 发送端流量控制
- 连接级 WINDOW_UPDATE 更新 `connection_send_window_`；溢出检查
- `send_data` 检查 `min(connection_send_window_, stream.send_window)` 后逐块发送；若为零则缓冲为 `PendingWrite`
- `flush_pending_writes()` 在 WINDOW_UPDATE 到达时排空
- RST_STREAM 时清理 pending write

### 5. 流状态机强制执行
- `validate_stream_state(stream_id, frame_type)` per RFC 7540 §5.1

### 6. Header List Size 强制执行
- HPACK 解码后计算 header list size；超过 `max_header_list_size` 发送 GOAWAY

### 7. 优雅 GOAWAY
- `close_gracefully()` 发送 GOAWAY(NO_ERROR) 并清空 pending writes

### 8. HPACK 动态表 — 编码器 & 解码器
- 完整动态表 + 驱逐 + 增量索引 + `set_max_table_size()` 用于 SETTINGS 集成

### 9. CONTINUATION 洪水防护
- `kMaxContinuationFrames=16` 每个头部块；超出则 GOAWAY(ENHANCE_YOUR_CALM)
- `kMaxHeaderBlockSize=256KB`；在初始 HEADERS payload 和每个 CONTINUATION append 后检查

### 10. PRIORITY 不打开流
- 替换 `get_or_create_stream()` 为直接 map lookup/emplace，保持流状态为 idle

### 11. Stream ID 单调性
- HEADERS 验证: stream_id 必须为奇数，且若 <= `last_stream_id_` 必须已存在
- DATA 和 CONTINUATION 使用 `streams_.find()` 而非 `get_or_create_stream()`（不得创建流）

### 12. Settings ACK 超时
- `awaiting_settings_ack_` 标志追踪；HttpServer h2 循环检查 30s 超时

### 13. 每流 HttpSessionContext
- `Http2AssembledStream` 有独立的 `stream_context`
- `maybe_reply_h2_via_dispatcher` 为每个流创建独立上下文

### 14. 常规头传播 + content-type 修复
- `parse_h2_header_block` 填充 `pseudo_headers` 和 `regular_headers` 两个 map
- 所有常规头（除 `host`, `content-length`, `transfer-encoding`）传播到 `HttpRequest`
- 移除硬编码 `"application/json"` content-type

### 15. 流式响应
- `maybe_reply_h2_via_dispatcher` 使用 `send_headers()` + `send_data()` 而非 `send_simple_response()`
- 响应 body 通过 `resp->body_begin()` / `resp->body_end()` 零拷贝访问

### 16. 请求体大小限制
- `Http2AssembledStream::kMaxBodySize = 10MB`
- `body_oversized` 标志；超限时响应 413

### 17. dispatch_h2_context 包含中间件 + 代理
- `dispatch_h2_context()` 包含 `global_pipeline_.execute()` 和 `proxy_->serve_proxy()`

### 18. Trailer 头支持
- `StreamEntry::received_data` 标志识别 trailer
- headers_bridge 处理 trailer 路径：仅合并常规头，丢弃伪头

### 19. Trailer 伪头拒绝
- Session 层：收到 DATA 后的 HEADERS 若含伪头，发送 GOAWAY(PROTOCOL_ERROR)

### 20. 客户端 preface 消费（ALPN h2 路径）
- `h2_handshake_active == true` 时首次读取消费 24 字节

### 21. mini_nginx enable_http2 配置
- 解析 `enable_http2` 字段
- `mini_nginx.json` 和 `http.json` 均已设置 `enable_http2: true`

### 22. kHuffTable 修正（本次会话）
- 用 Python hpack 库验证的 RFC 7541 正确数据替换了旧的错误 Huffman 表
- 旧表从 symbol 47 开始就与 RFC 不符（例如旧: sym47('/') code=0xfc bits=8, 正确: code=0x18 bits=6; 旧: sym48('0') code=0x18 bits=6, 正确: code=0x00 bits=5）
- 旧表 symbol 127-254 的数据也完全错误（大量重复的 0x3ffffffc/0x3ffffffd/0x3ffffffe/0x3fffffff 28-bit 模式），已替换为正确的 RFC 7541 值

### 23. Huffman EOS padding 处理改进（本次会话）
- `huffman_decode()` 末尾 padding 检测逻辑更新：沿 1-bit 路径追踪到 EOS symbol(-1) 则视为合法 padding

---

## 未完成 / 待修复

### 🔴 CRITICAL: HPACK 解码器 bug（当前阻塞）

kHuffTable 已修正，但 HPACK decode 仍然失败。可能原因：

1. **编译/链接未生效**：已确认 huffman_codec.cpp 重新编译了（cmake 输出 `Building CXX object ... huffman_codec.cpp.obj`），但需要验证运行时树是否正确。
2. **Huffman 解码逻辑 bug**：`huffman_decode()` 中树遍历可能有其他问题。注意 `HuffTree` 是运行时动态构建的（`build_tree()`），树节点数组只有 512 个，可能不够（257 个 symbol，最深 30 bit）。
3. **HPACK decoder 其他 bug**：`decode_prefixed_integer()` 或 `decode_string_literal()` 可能有边界条件错误。
   - `0x44` = literal with incremental indexing, name index = 4（6-bit prefix, 值 0x44 & 0x3f = 4）
   - `0x86` = huffman + length 6（7-bit prefix, 值 0x86 & 0x7f = 6）
   - 需要验证这两个 prefixed integer 的解析是否正确
4. **动态表相关**：decode 后 `add_to_dynamic_table()` 可能影响后续解码

#### 建议的排查方法
- 在 `hpack_decoder.cpp` 的 `decode()` 和 `decode_string_literal()` 中添加逐字节的调试日志，追踪每一步的 offset、解析结果
- 在 `huffman_decode()` 中添加调试日志，追踪树遍历过程
- 或写一个独立的 C++ 测试程序，直接用 hex 数据调用 `huffman_decode()` 和 `hpack_decoder_.decode()`，逐步追踪对比 Python 的解析结果
- 注意：session.cpp 中有临时调试日志，完成后需清理

### 🟡 Client preface 处理健壮性
- 当前实现盲目跳过 24 字节。如果 preface 分多个 TCP 读取到达，或 SETTINGS 帧与 preface 在同一读取中到达，当前代码可能无法正确处理
- 应验证 preface 内容并处理分片读取

### 🟡 更多互操作测试
- HPACK bug 修复后测试：
  - Python h2 GET 请求到各种路径
  - Python h2 POST 请求带 body
  - 多个并发流
  - 流量控制 window update
  - 如果可能，用 `h2spec` 测试

### 🟢 大响应流式传输
- 当前整个响应体缓冲后一次性 `send_data` 发送。对于大文件应通过流量控制分块

### 🟢 Server Push
- 当前 `enable_push=false`。可选的 h2 合规特性

---

## 项目架构备忘

### 命名空间
`yuan::net::http::http2`

### 构建命令
```bash
cmake -B build -DYUAN_ENABLE_HTTP_SSL=ON
cmake --build build --target mini_nginx -j 2
```

### 启动服务器（从项目根目录）
```bash
E:\test\server-lib\build\server\mini_nginx\mini_nginx.exe E:\test\server-lib\server\mini_nginx\mini_nginx.json
```

### 运行测试
```bash
python E:\test\server-lib\test_h2_client.py
```

### 关键注意事项
1. **LSP 误报**：clangd 报告大量 `cstddef file not found` / `Use of undeclared identifier 'std'` 等错误，这些是 LSP 假阳性，cmake 编译能成功。忽略 LSP 错误，以 cmake build 输出为准。
2. **预存在的链接错误**：`PluginLua` 和 `PluginTypeScript` 目标有 `_Unwind_Resume` 多重定义链接错误，与我们的修改无关。只构建 `Core`, `HttpProto`, `mini_nginx`, `ServerProxy`, `ServerServices`。
3. **内存限制**：构建环境内存有限，使用 `-j 1` 或 `-j 2` 避免OOM。
4. **Windows curl 无 HTTP/2**：系统 curl (Schannel) 不支持 HTTP/2。使用 Python `h2` 库测试。
5. **配置双路径**：`mini_nginx.json` 的 `enable_http2` 和 `http.json`(CWD) 的 `enable_http2` 都必须设为 true。

### 关键源文件

| 文件 | 说明 |
|------|------|
| `protocol/http/src/http2/huffman_codec.cpp` | Huffman 编解码，kHuffTable 已修正，decode 逻辑可能还有 bug |
| `protocol/http/src/http2/hpack_decoder.cpp` | HPACK 解码器，decode_prefixed_integer / decode_string_literal 可能有 bug |
| `protocol/http/src/http2/hpack_encoder.cpp` | HPACK 编码器，已完整重写 |
| `protocol/http/src/http2/session.cpp` | HTTP/2 会话，含临时调试日志 |
| `protocol/http/src/http_server.cpp` | HTTP 服务器，h2 模式入口、preface 处理 |
| `protocol/http/include/http2/session.h` | Session 头文件 |
| `protocol/http/include/http2/hpack_decoder.h` | HPACK 解码器头文件 |
| `protocol/http/include/http2/hpack_encoder.h` | HPACK 编码器头文件 |
| `protocol/http/include/http2/huffman_codec.h` | Huffman 编解码头文件 |
| `protocol/http/include/http2/types.h` | FrameType, ErrorCode, FrameHeader, Frame 结构体 |
| `protocol/http/include/http2/frame_codec.h` | 帧编解码 |
| `server/mini_nginx/main.cpp` | 服务器入口，enable_http2 配置解析 |
| `server/mini_nginx/mini_nginx.json` | 服务器配置 |
| `protocol/http/src/ops/http.json` | HTTP 功能配置 |
| `http.json` (项目根目录) | 运行时配置（从 ops/http.json 复制） |
| `test_h2_client.py` | Python h2 测试客户端 |

### TLS 证书
- `ca/ca.crt` — 自签名 TLS 证书
- `ca/ca.key` — 私钥

### Core SSL/TLS（先前会话修改）
- `core/core/include/net/secuity/ssl_handler.h` — `get_alpn_selected()` virtual
- `core/core/include/net/secuity/ssl_module.h` — `set_alpn_protocols()` virtual
- `core/core/include/net/secuity/openssl.h` — overrides
- `core/core/src/net/secuity/openssl.cpp` — ALPN 实现
