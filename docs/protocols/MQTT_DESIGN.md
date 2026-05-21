# MQTT 协议实现设计文档

## 1. 概述

本文档描述在 server-lib 框架中实现 MQTT v3.1.1 / v5.0 协议服务端的完整设计。
MQTT（Message Queuing Telemetry Transport）是轻量级发布/订阅消息传输协议，广泛用于 IoT、移动推送、实时数据流等场景。

### 1.1 目标

- 完整支持 MQTT v3.1.1（OASIS Standard）和 v5.0（OASIS Standard）
- 支持 QoS 0 / 1 / 2 全部消息投递保证
- 支持主题通配符订阅（`+` 单级、`#` 多级）
- 支持遗嘱消息（Will Message）
- 支持保留消息（Retained Message）
- 支持共享订阅（MQTT 5.0）
- 支持主题别名（Topic Alias，MQTT 5.0）
- 支持消息过期（Message Expiry Interval，MQTT 5.0）
- 支持会话过期（Session Expiry Interval，MQTT 5.0）
- 支持流量控制（Receive Maximum，MQTT 5.0）
- 支持增强认证（Enhanced Auth，MQTT 5.0）
- 支持请求/响应模式（Request/Response，MQTT 5.0）
- 支持用户属性（User Property，MQTT 5.0）
- 支持载荷格式指示（Payload Format Indicator，MQTT 5.0）
- 支持 TLS 加密传输
- 高性能：异步非阻塞 + C++20 协程
- 可扩展：Handler 回调接口允许业务层完全控制行为

### 1.2 不在范围内

- MQTT-SN（传感器网络变体）
- MQTT over WebSocket（由上层 WebSocket 协议处理，本层只关注纯 TCP 上的 MQTT）
- 集群/桥接模式（单节点实现，可后续扩展）

## 2. 协议基础

### 2.1 报文格式

```
+--------+-------------------+------------------+
| Fixed  | Variable          | Payload          |
| Header | Header            |                  |
+--------+-------------------+------------------+
| 1 byte | 0+ bytes          | 0+ bytes         |
+--------+-------------------+------------------+

Fixed Header:
  Bit    7-4: Packet Type
  Bit      3: Flags (DUP / QoS / RETAIN for PUBLISH)
  Bit    2-1: QoS Level (PUBLISH only)
  Bit      0: RETAIN (PUBLISH only)

  Remaining Length: 1-4 bytes variable-length encoding
    Value 0-127:        1 byte
    Value 128-16383:    2 bytes
    Value 16384-2097151: 3 bytes
    Value 2097152-268435455: 4 bytes
```

### 2.2 报文类型

| Type | Value | v3.1.1 | v5.0 | 方向 |
|------|-------|--------|------|------|
| CONNECT | 1 | ✅ | ✅ | C→S |
| CONNACK | 2 | ✅ | ✅ | S→C |
| PUBLISH | 3 | ✅ | ✅ | 双向 |
| PUBACK | 4 | ✅ | ✅ | 双向 |
| PUBREC | 5 | ✅ | ✅ | 双向 |
| PUBREL | 6 | ✅ | ✅ | 双向 |
| PUBCOMP | 7 | ✅ | ✅ | 双向 |
| SUBSCRIBE | 8 | ✅ | ✅ | C→S |
| SUBACK | 9 | ✅ | ✅ | S→C |
| UNSUBSCRIBE | 10 | ✅ | ✅ | C→S |
| UNSUBACK | 11 | ✅ | ✅ | S→C |
| PINGREQ | 12 | ✅ | ✅ | C→S |
| PINGRESP | 13 | ✅ | ✅ | S→C |
| DISCONNECT | 14 | ✅ | ✅ | 双向 |
| AUTH | 15 | ❌ | ✅ | 双向 |

### 2.3 QoS 级别

| QoS | 保证 | 流程 |
|-----|------|------|
| 0 | 最多一次 | PUBLISH → (无确认) |
| 1 | 至少一次 | PUBLISH → PUBACK |
| 2 | 恰好一次 | PUBLISH → PUBREC → PUBREL → PUBCOMP |

### 2.4 MQTT 5.0 Properties

MQTT 5.0 在可变头部后增加了 Properties 字段：

```
+----------------------+------------------+
| Property Length      | Properties       |
| (Variable Byte Int) |                  |
+----------------------+------------------+
```

| ID  | Name | Type | 出现位置 |
|-----|------|------|----------|
| 0x01 | Payload Format Indicator | Byte | PUBLISH, CONNECT(Will) |
| 0x02 | Message Expiry Interval | 4 Byte Int | PUBLISH |
| 0x03 | Content Type | UTF-8 String | PUBLISH, CONNECT(Will) |
| 0x08 | Response Topic | UTF-8 String | PUBLISH |
| 0x09 | Correlation Data | Binary Data | PUBLISH |
| 0x0B | Subscription Identifier | Variable Byte Int | SUBSCRIBE |
| 0x11 | Session Expiry Interval | 4 Byte Int | CONNECT, CONNACK, DISCONNECT |
| 0x12 | Assigned Client Identifier | UTF-8 String | CONNACK |
| 0x13 | Server Keep Alive | 2 Byte Int | CONNACK |
| 0x15 | Authentication Method | UTF-8 String | CONNECT, CONNACK, AUTH |
| 0x16 | Authentication Data | Binary Data | CONNECT, CONNACK, AUTH |
| 0x17 | Request Problem Information | Byte | CONNECT |
| 0x18 | Will Delay Interval | 4 Byte Int | CONNECT |
| 0x19 | Request Response Information | Byte | CONNECT |
| 0x1A | Response Information | UTF-8 String | CONNACK |
| 0x1C | Server Reference | UTF-8 String | CONNACK, DISCONNECT |
| 0x1F | Reason String | UTF-8 String | 所有 ACK |
| 0x21 | Receive Maximum | 2 Byte Int | CONNECT, CONNACK |
| 0x22 | Topic Alias Maximum | 2 Byte Int | CONNECT, CONNACK |
| 0x23 | Topic Alias | 2 Byte Int | PUBLISH |
| 0x24 | Maximum QoS | Byte | CONNACK |
| 0x25 | Retain Available | Byte | CONNACK |
| 0x26 | User Property | UTF-8 String Pair | 所有 |
| 0x27 | Maximum Packet Size | 4 Byte Int | CONNECT, CONNACK |
| 0x28 | Wildcard Subscription Available | Byte | CONNACK |
| 0x29 | Subscription Identifier Available | Byte | CONNACK |
| 0x2A | Shared Subscription Available | Byte | CONNACK |

### 2.5 Reason Codes（MQTT 5.0）

关键 Reason Code：

| Code | Name | 使用位置 |
|------|------|----------|
| 0x00 | Success | CONNACK, PUBACK, PUBREC, PUBREL, PUBCOMP, UNSUBACK, AUTH |
| 0x01 | Granted QoS 1 | SUBACK |
| 0x02 | Granted QoS 2 | SUBACK |
| 0x04 | Disconnect with Will Message | DISCONNECT |
| 0x10 | No matching subscribers | PUBLISH |
| 0x11 | No subscription existed | UNSUBACK |
| 0x18 | Continue Authentication | AUTH |
| 0x19 | Re-authenticate | AUTH |
| 0x80 | Unspecified error | 通用 |
| 0x81 | Malformed Packet | 通用 |
| 0x82 | Protocol Error | 通用 |
| 0x83 | Implementation specific error | 通用 |
| 0x84 | Unsupported Protocol Version | CONNACK |
| 0x85 | Client Identifier not valid | CONNACK |
| 0x86 | Bad User Name or Password | CONNACK |
| 0x87 | Not authorized | 通用 |
| 0x89 | Server busy | CONNACK |
| 0x8A | Banned | CONNACK |
| 0x8C | Bad authentication method | CONNACK, AUTH |
| 0x8E | Topic Name invalid | CONNACK, PUBACK, SUBACK |
| 0x90 | Packet Identifier in use | PUBACK, PUBREC, SUBACK, UNSUBACK |
| 0x91 | Packet Identifier not found | PUBREL, PUBCOMP |
| 0x93 | Receive Maximum exceeded | 通用 |
| 0x94 | Topic Alias invalid | PUBACK |
| 0x95 | Packet too large | 通用 |
| 0x97 | Quota exceeded | 通用 |
| 0x99 | Shared Subscriptions not supported | SUBACK |
| 0x9A | Subscription Identifiers not supported | SUBACK |
| 0x9B | Wildcard Subscriptions not supported | SUBACK |

## 3. 架构设计

### 3.1 模块划分

```
protocol/mqtt/
├── CMakeLists.txt
├── include/
│   ├── mqtt.h                         # 统一头文件
│   ├── mqtt_protocol.h                # 常量、枚举、Reason Code、Property ID
│   ├── mqtt_packet.h                  # 所有报文结构体定义
│   ├── mqtt_properties.h              # MQTT 5.0 Properties 解析/编码
│   ├── mqtt_codec.h                   # 报文编解码器
│   ├── mqtt_config.h                  # 服务器配置
│   ├── mqtt_handler.h                 # 业务回调接口
│   ├── mqtt_session.h                 # 会话状态管理
│   ├── mqtt_topic_tree.h              # 主题订阅树（通配符匹配）
│   ├── mqtt_retained_store.h          # 保留消息存储
│   ├── mqtt_dispatcher.h              # 命令分发/处理
│   └── mqtt_server.h                  # 服务器核心
└── src/
    ├── mqtt_codec.cpp                 # 编解码实现
    ├── mqtt_properties.cpp            # Properties 解析实现
    ├── mqtt_session.cpp               # 会话管理实现
    ├── mqtt_topic_tree.cpp            # 主题树实现
    ├── mqtt_retained_store.cpp        # 保留消息存储实现
    ├── mqtt_dispatcher.cpp            # 命令处理实现
    └── mqtt_server.cpp                # 服务器实现
```

### 3.2 类图

```
┌─────────────┐     ┌──────────────┐     ┌─────────────────┐
│  MqttServer  │────>│ MqttDispatcher│────>│  MqttSessionMgr │
└──────┬───────┘     └──────┬───────┘     └────────┬────────┘
       │                    │                      │
       │                    ├──────────────────────┤
       │                    │                      │
       ▼                    ▼                      ▼
┌─────────────┐     ┌──────────────┐     ┌─────────────────┐
│ MqttCodec   │     │ MqttTopicTree│     │  MqttSession    │
└─────────────┘     └──────────────┘     └─────────────────┘
                          │
                          ▼
                    ┌──────────────────┐
                    │MqttRetainedStore │
                    └──────────────────┘
```

### 3.3 数据流

```
Client TCP → AsyncListenerHost → handle_connection()
    → co_await ctx.read_async()
    → MqttCodec::try_decode()  ──→ 完整报文？→ MqttDispatcher::dispatch()
    │                                               │
    │   ┌───────────────────────────────────────────┘
    │   ▼
    │   处理报文：
    │   - CONNECT  → 验证 → CONNACK → 建立会话
    │   - PUBLISH  → 主题匹配 → 分发给订阅者
    │   - SUBSCRIBE → 注册订阅 → SUBACK
    │   - UNSUBSCRIBE → 注销订阅 → UNSUBACK
    │   - PINGREQ → PINGRESP
    │   - DISCONNECT → 清理会话
    │   - QoS 确认流 → PUBACK/PUBREC/PUBREL/PUBCOMP
    │
    └── 不完整 → 缓存等待更多数据
```

## 4. 详细设计

### 4.1 mqtt_protocol.h — 协议常量

```cpp
namespace yuan::net::mqtt {

// 报文类型
enum class PacketType : uint8_t {
    CONNECT    = 1,  CONNACK    = 2,
    PUBLISH    = 3,  PUBACK     = 4,
    PUBREC     = 5,  PUBREL     = 6,
    PUBCOMP    = 7,  SUBSCRIBE  = 8,
    SUBACK     = 9,  UNSUBSCRIBE = 10,
    UNSUBACK   = 11, PINGREQ    = 12,
    PINGRESP   = 13, DISCONNECT = 14,
    AUTH       = 15
};

// QoS
enum class QoS : uint8_t { AT_MOST_ONCE = 0, AT_LEAST_ONCE = 1, EXACTLY_ONCE = 2 };

// Connect Return Code (v3.1.1) / Reason Code (v5.0 CONNACK)
enum class ConnackCode : uint8_t { ... };

// SUBACK Reason Code
enum class SubackReason : uint8_t { ... };

// Property IDs (MQTT 5.0)
enum class PropertyId : uint8_t { ... };

// Protocol Level
enum class ProtocolLevel : uint8_t { V3_1_1 = 4, V5_0 = 5 };

} // namespace yuan::net::mqtt
```

### 4.2 mqtt_packet.h — 报文结构体

所有报文以结构体表示，包含固定头部字段 + 可变头部字段 + 载荷。

```cpp
struct MqttFixedHeader {
    PacketType type;
    uint8_t flags;       // DUP, QoS, RETAIN
    uint32_t remaining_length;
};

struct MqttConnectPacket {
    // Variable Header
    ProtocolLevel protocol_level;
    uint8_t connect_flags;
    uint16_t keep_alive;

    // Payload
    std::string client_id;
    std::optional<std::string> will_topic;
    std::optional<std::vector<uint8_t>> will_payload;
    std::optional<std::string> username;
    std::optional<std::string> password;

    // MQTT 5.0 Properties (connect)
    std::optional<uint32_t> session_expiry_interval;
    std::optional<uint16_t> receive_maximum;
    std::optional<uint32_t> maximum_packet_size;
    std::optional<uint16_t> topic_alias_maximum;
    std::optional<uint8_t> request_response_information;
    std::optional<uint8_t> request_problem_information;
    std::optional<std::string> authentication_method;
    std::optional<std::vector<uint8_t>> authentication_data;
    std::vector<UserProperty> user_properties;

    // MQTT 5.0 Will Properties
    std::optional<uint32_t> will_delay_interval;
    std::optional<uint8_t> payload_format_indicator;
    std::optional<uint32_t> message_expiry_interval;
    std::optional<std::string> content_type;
    std::optional<std::string> response_topic;
    std::optional<std::vector<uint8_t>> correlation_data;
    std::vector<UserProperty> will_user_properties;
};

struct MqttConnackPacket {
    uint8_t session_present;     // Connect Acknowledge Flags
    uint8_t reason_code;         // Connect Return Code / Reason Code
    // MQTT 5.0 Properties
    std::optional<uint32_t> session_expiry_interval;
    std::optional<uint16_t> receive_maximum;
    std::optional<uint32_t> maximum_packet_size;
    std::optional<uint16_t> topic_alias_maximum;
    std::optional<uint8_t> maximum_qos;
    std::optional<uint8_t> retain_available;
    std::optional<std::string> assigned_client_id;
    std::optional<uint16_t> server_keep_alive;
    std::optional<std::string> response_information;
    std::optional<std::string> server_reference;
    std::optional<std::string> reason_string;
    std::optional<uint8_t> wildcard_subscription_available;
    std::optional<uint8_t> subscription_identifier_available;
    std::optional<uint8_t> shared_subscription_available;
    std::vector<UserProperty> user_properties;
};

struct MqttPublishPacket {
    uint8_t dup;
    QoS qos;
    uint8_t retain;
    std::string topic;
    std::optional<uint16_t> packet_id;    // QoS 1/2
    std::vector<uint8_t> payload;
    // MQTT 5.0 Properties
    std::optional<uint8_t> payload_format_indicator;
    std::optional<uint32_t> message_expiry_interval;
    std::optional<uint16_t> topic_alias;
    std::optional<std::string> response_topic;
    std::optional<std::vector<uint8_t>> correlation_data;
    std::vector<UserProperty> user_properties;
    std::optional<std::string> content_type;
    std::optional<uint32_t> subscription_identifier;  // 仅入站
};

struct MqttSubscribePacket {
    uint16_t packet_id;
    struct Subscription {
        std::string topic_filter;
        QoS maximum_qos;          // v3.1.1: requested QoS
        uint8_t no_local;         // v5.0
        uint8_t retain_as_published; // v5.0
        uint8_t retain_handling;  // v5.0
    };
    std::vector<Subscription> subscriptions;
    // MQTT 5.0 Properties
    std::optional<uint32_t> subscription_identifier;
    std::vector<UserProperty> user_properties;
};

// ... 其他报文结构体类似
```

### 4.3 mqtt_codec.h — 编解码器

编解码器负责在字节流和报文结构体之间转换。

#### 核心接口

```cpp
class MqttCodec {
public:
    // 从 ByteBuffer 中尝试解析一个完整报文
    // 返回 {报文类型, 消费的字节数} 或 nullopt（数据不完整/格式错误）
    static std::optional<std::pair<PacketType, size_t>>
    try_decode(const uint8_t *data, size_t len);

    // 解码各类报文
    static std::optional<MqttConnectPacket>   decode_connect(const uint8_t *data, size_t len);
    static std::optional<MqttPublishPacket>   decode_publish(const uint8_t *data, size_t len, uint8_t flags);
    static std::optional<MqttSubscribePacket> decode_subscribe(const uint8_t *data, size_t len, ProtocolLevel level);
    // ... 其他 decode

    // 编码各类报文
    static ByteBuffer encode_connack(const MqttConnackPacket &pkt, ProtocolLevel level);
    static ByteBuffer encode_publish(const MqttPublishPacket &pkt, ProtocolLevel level);
    static ByteBuffer encode_suback(uint16_t packet_id, const std::vector<uint8_t> &reason_codes, ProtocolLevel level);
    static ByteBuffer encode_puback(uint16_t packet_id, uint8_t reason_code, ProtocolLevel level);
    static ByteBuffer encode_pubrec(uint16_t packet_id, uint8_t reason_code, ProtocolLevel level);
    static ByteBuffer encode_pubrel(uint16_t packet_id, uint8_t reason_code, ProtocolLevel level);
    static ByteBuffer encode_pubcomp(uint16_t packet_id, uint8_t reason_code, ProtocolLevel level);
    static ByteBuffer encode_unsuback(uint16_t packet_id, const std::vector<uint8_t> &reason_codes, ProtocolLevel level);
    static ByteBuffer encode_pingresp();
    static ByteBuffer encode_disconnect(uint8_t reason_code, ProtocolLevel level);

private:
    // Variable Byte Integer 编解码
    static size_t encode_remaining_length(uint32_t value, uint8_t *out);
    static std::optional<uint32_t> decode_remaining_length(const uint8_t *data, size_t len, size_t &bytes_consumed);

    // UTF-8 Encoded String 编解码
    static std::optional<std::string> read_utf8_string(const uint8_t *data, size_t len, size_t &offset);
    static void write_utf8_string(ByteBuffer &buf, const std::string &str);

    // Binary Data 编解码
    static std::optional<std::vector<uint8_t>> read_binary_data(const uint8_t *data, size_t len, size_t &offset);
    static void write_binary_data(ByteBuffer &buf, const std::vector<uint8_t> &data);

    // Properties 编解码（MQTT 5.0）
    static size_t decode_properties(const uint8_t *data, size_t len, MqttProperties &props);
    static ByteBuffer encode_properties(const MqttProperties &props);

    // Fixed Header 构建
    static ByteBuffer build_fixed_header(PacketType type, uint8_t flags, size_t remaining_length);
};
```

#### 关键设计点

1. **TCP 粘包处理**：`try_decode` 只解析固定头部（1+4 字节）确定报文边界，不消费数据。调用方根据返回的 `size_t` 确定完整报文长度后，再调用具体 `decode_*` 解码。
2. **MQTT 字符串长度是 UTF-8 大端序前缀**：2 字节 length + string data，与项目 ByteBuffer 的大端序默认行为一致。
3. **Variable Byte Integer**：MQTT 特有的变长编码，每个字节的 bit7 是续标志，bit6-0 是有效位。
4. **Protocol Level 分支**：v3.1.1 和 v5.0 的报文格式差异主要在 Properties 和 Reason Code 字段，编码时根据 `ProtocolLevel` 分支处理。

### 4.4 mqtt_properties.h — MQTT 5.0 Properties

```cpp
struct UserProperty {
    std::string key;
    std::string value;
};

struct MqttProperties {
    std::optional<uint8_t>    payload_format_indicator;
    std::optional<uint32_t>   message_expiry_interval;
    std::optional<std::string> content_type;
    std::optional<std::string> response_topic;
    std::optional<std::vector<uint8_t>> correlation_data;
    std::optional<uint32_t>   subscription_identifier;
    std::optional<uint32_t>   session_expiry_interval;
    std::optional<std::string> assigned_client_identifier;
    std::optional<uint16_t>   server_keep_alive;
    std::optional<std::string> authentication_method;
    std::optional<std::vector<uint8_t>> authentication_data;
    std::optional<uint8_t>    request_problem_information;
    std::optional<uint32_t>   will_delay_interval;
    std::optional<uint8_t>    request_response_information;
    std::optional<std::string> response_information;
    std::optional<std::string> server_reference;
    std::optional<std::string> reason_string;
    std::optional<uint16_t>   receive_maximum;
    std::optional<uint16_t>   topic_alias_maximum;
    std::optional<uint16_t>   topic_alias;
    std::optional<uint8_t>    maximum_qos;
    std::optional<uint8_t>    retain_available;
    std::optional<uint32_t>   maximum_packet_size;
    std::optional<uint8_t>    wildcard_subscription_available;
    std::optional<uint8_t>    subscription_identifier_available;
    std::optional<uint8_t>    shared_subscription_available;
    std::vector<UserProperty> user_properties;

    // 计算编码后的总字节数（用于写 Property Length）
    size_t encoded_size() const;
    // 判断是否有任何属性被设置
    bool has_any() const;
};
```

### 4.5 mqtt_session.h — 会话管理

```cpp
enum class MqttSessionState {
    disconnected,
    connecting,
    connected,
    disconnecting
};

struct MqttWillMessage {
    std::string topic;
    std::vector<uint8_t> payload;
    QoS qos;
    bool retain;
    // MQTT 5.0
    std::optional<uint32_t> will_delay_interval;
    std::optional<uint8_t>  payload_format_indicator;
    std::optional<uint32_t> message_expiry_interval;
    std::optional<std::string> content_type;
    std::optional<std::string> response_topic;
    std::optional<std::vector<uint8_t>> correlation_data;
    std::vector<UserProperty> user_properties;
};

class MqttSession {
public:
    explicit MqttSession(TcpConnection *conn);

    // 基础信息
    uint64_t session_id() const;
    const std::string &client_id() const;
    void set_client_id(const std::string &id);
    ProtocolLevel protocol_level() const;
    void set_protocol_level(ProtocolLevel level);
    MqttSessionState state() const;
    void set_state(MqttSessionState state);

    // Keep Alive
    uint16_t keep_alive() const;
    void set_keep_alive(uint16_t seconds);
    void update_last_activity();

    // 订阅
    const std::vector<std::string> &subscriptions() const;
    void add_subscription(const std::string &topic_filter, QoS qos);
    void remove_subscription(const std::string &topic_filter);
    QoS subscription_qos(const std::string &topic_filter) const;

    // QoS 2 状态机
    bool add_inflight_packet_id(uint16_t pid);          // PUBREC 阶段
    bool has_inflight_packet_id(uint16_t pid) const;
    void remove_inflight_packet_id(uint16_t pid);       // PUBCOMP 阶段
    uint16_t next_packet_id();

    // QoS 2 出站
    void add_outgoing_packet_id(uint16_t pid);
    bool has_outgoing_packet_id(uint16_t pid) const;
    void remove_outgoing_packet_id(uint16_t pid);

    // 遗嘱消息
    const MqttWillMessage *will_message() const;
    void set_will_message(MqttWillMessage will);
    void clear_will_message();

    // MQTT 5.0 Session
    bool clean_start() const;
    void set_clean_start(bool clean);
    uint32_t session_expiry_interval() const;
    void set_session_expiry_interval(uint32_t interval);

    // MQTT 5.0 Flow Control
    uint16_t receive_maximum() const;
    void set_receive_maximum(uint16_t max);
    uint16_t client_receive_maximum() const;
    void set_client_receive_maximum(uint16_t max);
    uint32_t maximum_packet_size() const;
    void set_maximum_packet_size(uint32_t size);
    uint16_t topic_alias_maximum() const;
    void set_topic_alias_maximum(uint16_t max);

    // MQTT 5.0 Topic Alias
    void set_topic_alias(uint16_t alias, const std::string &topic);
    std::optional<std::string> resolve_topic_alias(uint16_t alias) const;

    // 连接
    TcpConnection *connection() const;

private:
    uint64_t session_id_;
    std::string client_id_;
    ProtocolLevel protocol_level_ = ProtocolLevel::V3_1_1;
    MqttSessionState state_ = MqttSessionState::disconnected;
    uint16_t keep_alive_ = 60;
    std::chrono::steady_clock::time_point last_activity_;

    std::map<std::string, QoS> subscriptions_;

    uint16_t next_packet_id_ = 1;
    std::set<uint16_t> inflight_packet_ids_;      // QoS 2 入站: PUBREC 已发
    std::set<uint16_t> outgoing_packet_ids_;      // QoS 2 出站: PUBREL 等待

    std::optional<MqttWillMessage> will_message_;

    bool clean_start_ = true;
    uint32_t session_expiry_interval_ = 0;
    uint16_t receive_maximum_ = 65535;
    uint16_t client_receive_maximum_ = 65535;
    uint32_t maximum_packet_size_ = 0;             // 0 = 无限制
    uint16_t topic_alias_maximum_ = 0;

    std::map<uint16_t, std::string> topic_aliases_;

    TcpConnection *conn_;
};

class MqttSessionManager {
public:
    MqttSession &create_session(TcpConnection *conn);
    void remove_session(uint64_t session_id);
    MqttSession *find_by_client_id(const std::string &client_id);
    MqttSession *find_by_connection(TcpConnection *conn);
    std::vector<MqttSession *> all_sessions();

    // 清理过期会话（MQTT 5.0 Session Expiry）
    void cleanup_expired();

private:
    std::map<uint64_t, std::unique_ptr<MqttSession>> sessions_;
    std::map<std::string, uint64_t> client_id_index_;
    uint64_t next_session_id_ = 1;
};
```

### 4.6 mqtt_topic_tree.h — 主题订阅树

使用多级树结构实现高效的主题匹配，支持 `+` 和 `#` 通配符。

```cpp
struct MqttSubscription {
    uint64_t session_id;
    QoS qos;
    uint8_t no_local;              // MQTT 5.0
    uint8_t retain_as_published;   // MQTT 5.0
    uint8_t retain_handling;       // MQTT 5.0
    std::optional<uint32_t> subscription_identifier; // MQTT 5.0
};

class MqttTopicTree {
public:
    // 添加订阅，返回被替换的 QoS（如果已有）
    std::optional<QoS> subscribe(const std::string &topic_filter,
                                  const MqttSubscription &sub);

    // 取消订阅
    bool unsubscribe(const std::string &topic_filter, uint64_t session_id);

    // 匹配主题，返回所有匹配的订阅
    std::vector<MqttSubscription> match(const std::string &topic) const;

    // 获取某 session 的所有订阅
    std::vector<std::string> subscriptions(uint64_t session_id) const;

    // 移除某 session 的所有订阅
    void remove_all(uint64_t session_id);

    // 验证主题过滤器是否合法
    static bool validate_topic_filter(const std::string &filter);

    // 验证主题名是否合法（不能包含通配符）
    static bool validate_topic_name(const std::string &topic);

private:
    struct Node {
        std::map<std::string, Node> children;       // 精确匹配子节点
        std::unique_ptr<Node> single_level_wildcard; // '+' 通配符子节点
        std::unique_ptr<Node> multi_level_wildcard;  // '#' 通配符子节点
        std::vector<MqttSubscription> subscriptions; // 此节点上的订阅者
    };

    Node root_;

    void match_recursive(const Node &node, const std::vector<std::string> &levels,
                         size_t level_index, std::vector<MqttSubscription> &result) const;
    Node *find_or_create_node(const std::string &topic_filter);
};
```

#### 匹配算法

主题名按 `/` 分割为层级。从根节点开始，对每个层级依次匹配：

1. **精确匹配**：先在 `children` 中查找当前层级名
2. **单级通配符**：如果有 `+` 节点，也递归匹配下一层级
3. **多级通配符**：如果有 `#` 节点，直接收集该节点的所有订阅者（`#` 匹配剩余所有层级）

### 4.7 mqtt_retained_store.h — 保留消息存储

```cpp
struct MqttRetainedMessage {
    std::string topic;
    std::vector<uint8_t> payload;
    QoS qos;
    std::chrono::steady_clock::time_point stored_time;
    // MQTT 5.0
    std::optional<uint32_t> message_expiry_interval;
    std::optional<uint8_t> payload_format_indicator;
    std::optional<std::string> content_type;
    std::vector<UserProperty> user_properties;
};

class MqttRetainedStore {
public:
    // 存储/更新保留消息（payload 为空则删除）
    void store(const MqttRetainedMessage &msg);

    // 获取匹配指定主题过滤器的保留消息
    std::vector<MqttRetainedMessage> match(const std::string &topic_filter) const;

    // 清理过期保留消息
    void cleanup_expired();

    size_t size() const;

private:
    std::map<std::string, MqttRetainedMessage> messages_;  // topic → message
};
```

### 4.8 mqtt_handler.h — 业务回调接口

```cpp
class MqttHandler {
public:
    virtual ~MqttHandler() = default;

    // 连接认证
    virtual bool on_connect(MqttSession *session, const std::string &client_id,
                           const std::string &username, const std::string &password) {
        return true;
    }

    // 增强认证（MQTT 5.0）
    virtual bool on_auth(MqttSession *session, const std::string &method,
                        const std::vector<uint8_t> &data) {
        return false;
    }

    // 客户端已连接
    virtual void on_connected(MqttSession *session) {}

    // 客户端断开
    virtual void on_disconnected(MqttSession *session, uint8_t reason_code) {}

    // 发布消息（由客户端发来）
    virtual bool on_publish(MqttSession *session, const std::string &topic,
                           const std::vector<uint8_t> &payload, QoS qos, bool retain) {
        return true;
    }

    // 订阅请求
    virtual bool on_subscribe(MqttSession *session, const std::string &topic_filter, QoS qos) {
        return true;
    }

    // 取消订阅
    virtual void on_unsubscribe(MqttSession *session, const std::string &topic_filter) {}

    // 消息已投递完成（QoS 1 PUBACK / QoS 2 PUBCOMP）
    virtual void on_message_delivered(MqttSession *session, uint16_t packet_id) {}
};
```

### 4.9 mqtt_config.h — 服务器配置

```cpp
struct MqttServerConfig {
    uint16_t port = 1883;
    uint16_t tls_port = 8883;               // 0 = 不启用 TLS
    uint32_t max_connections = 10000;
    uint32_t max_message_size = 256 * 1024;  // 256KB
    uint32_t max_packet_size = 256 * 1024;   // MQTT 5.0 Maximum Packet Size
    uint16_t keep_alive_default = 60;        // 默认 Keep Alive
    double keep_alive_factor = 1.5;          // Keep Alive 容忍系数
    uint16_t topic_alias_maximum = 0;        // 0 = 不支持
    uint16_t receive_maximum = 65535;        // 流控窗口
    uint8_t maximum_qos = 2;                 // 服务器支持的最高 QoS
    bool retain_available = true;
    bool wildcard_subscription_available = true;
    bool subscription_identifier_available = true;
    bool shared_subscription_available = true;
    bool require_authentication = false;
    uint32_t idle_timeout_ms = 0;            // 0 = 使用 keep_alive * factor
    std::vector<ProtocolLevel> supported_versions = {
        ProtocolLevel::V3_1_1, ProtocolLevel::V5_0
    };
    std::string default_auth_method;         // MQTT 5.0 增强认证方法
};
```

### 4.10 mqtt_dispatcher.h — 命令分发

```cpp
class MqttDispatcher {
public:
    MqttDispatcher(const MqttServerConfig &config,
                   MqttSessionManager &session_mgr,
                   MqttTopicTree &topic_tree,
                   MqttRetainedStore &retained_store,
                   MqttHandler *handler = nullptr);

    // 处理一个完整报文，返回需要发送的响应（可能为空）
    ByteBuffer dispatch(MqttSession &session, const uint8_t *data, size_t len);

    // 会话断开时清理
    void on_session_closed(MqttSession &session);

    // 向指定会话推送消息（服务端主动 PUBLISH）
    ByteBuffer build_publish_for_session(MqttSession &session,
                                          const std::string &topic,
                                          const std::vector<uint8_t> &payload,
                                          QoS qos, bool retain);

private:
    ByteBuffer handle_connect(MqttSession &session, const uint8_t *data, size_t len);
    ByteBuffer handle_publish(MqttSession &session, const uint8_t *data, size_t len, uint8_t flags);
    ByteBuffer handle_puback(MqttSession &session, const uint8_t *data, size_t len);
    ByteBuffer handle_pubrec(MqttSession &session, const uint8_t *data, size_t len);
    ByteBuffer handle_pubrel(MqttSession &session, const uint8_t *data, size_t len);
    ByteBuffer handle_pubcomp(MqttSession &session, const uint8_t *data, size_t len);
    ByteBuffer handle_subscribe(MqttSession &session, const uint8_t *data, size_t len);
    ByteBuffer handle_unsubscribe(MqttSession &session, const uint8_t *data, size_t len);
    ByteBuffer handle_pingreq(MqttSession &session);
    ByteBuffer handle_disconnect(MqttSession &session, const uint8_t *data, size_t len);
    ByteBuffer handle_auth(MqttSession &session, const uint8_t *data, size_t len);

    // 将 PUBLISH 分发给所有匹配的订阅者
    void publish_to_subscribers(MqttSession &source_session,
                                const MqttPublishPacket &pkt);

    // 发送遗嘱消息
    void send_will_message(MqttSession &session);

    const MqttServerConfig &config_;
    MqttSessionManager &session_mgr_;
    MqttTopicTree &topic_tree_;
    MqttRetainedStore &retained_store_;
    MqttHandler *handler_;
};
```

### 4.11 mqtt_server.h — 服务器核心

```cpp
class MqttServer {
public:
    MqttServer();
    explicit MqttServer(const MqttServerConfig &config);
    ~MqttServer();

    MqttServer(const MqttServer &) = delete;
    MqttServer &operator=(const MqttServer &) = delete;

    bool init(int port);
    bool init(int port, NetworkRuntime &runtime);
    void serve();
    void stop();

    NetworkRuntime *runtime() const noexcept;
    void set_handler(MqttHandler *handler);
    const MqttServerConfig &config() const;

    // 向所有订阅某主题的客户端发布消息
    void publish(const std::string &topic, const std::vector<uint8_t> &payload,
                 QoS qos = QoS::AT_MOST_ONCE, bool retain = false);

    // 获取统计信息
    size_t connected_clients() const;

private:
    coroutine::Task<void> handle_connection(AsyncConnectionContext ctx);
    void on_keepalive_timeout(MqttSession &session);

    AsyncListenerHost listener_;
    std::unique_ptr<NetworkRuntime> owned_runtime_;
    MqttServerConfig config_;

    MqttSessionManager session_mgr_;
    MqttTopicTree topic_tree_;
    MqttRetainedStore retained_store_;
    MqttDispatcher dispatcher_;
    MqttHandler *handler_ = nullptr;
};
```

#### handle_connection 流程

```cpp
coroutine::Task<void> MqttServer::handle_connection(AsyncConnectionContext ctx) {
    auto &session = session_mgr_.create_session(ctx.native_handle());
    session.set_state(MqttSessionState::connecting);

    ByteBuffer recv_buf;
    bool first_packet = true;

    while (ctx.is_connected()) {
        auto read_result = co_await ctx.read_async(config_.idle_timeout_ms);
        if (read_result.status != coroutine::IoStatus::success) {
            break;
        }

        recv_buf.append(read_result.data);
        session.update_last_activity();

        while (recv_buf.readable_bytes() > 0) {
            auto decoded = MqttCodec::try_decode(
                reinterpret_cast<const uint8_t *>(recv_buf.read_ptr()),
                recv_buf.readable_bytes());
            if (!decoded) break;  // 数据不完整，等待更多

            auto [type, pkt_len] = *decoded;

            // 第一个包必须是 CONNECT
            if (first_packet && type != PacketType::CONNECT) {
                ctx.close();
                co_return;
            }

            auto response = dispatcher_.dispatch(session,
                reinterpret_cast<const uint8_t *>(recv_buf.read_ptr()), pkt_len);

            if (type == PacketType::CONNECT) {
                first_packet = false;
            }

            recv_buf.consume(pkt_len);

            if (response.readable_bytes() > 0) {
                co_await ctx.write_async(std::move(response));
            }

            // DISCONNECT 后结束
            if (type == PacketType::DISCONNECT &&
                session.state() == MqttSessionState::disconnected) {
                ctx.close();
                co_return;
            }
        }
    }

    // 连接断开 → 发送遗嘱消息 + 清理会话
    dispatcher_.on_session_closed(session);
    session_mgr_.remove_session(session.session_id());
    ctx.close();
    co_return;
}
```

## 5. QoS 2 状态机

QoS 2 是最复杂的投递保证，需要两端维护状态：

```
发布者                        接收者
  │                            │
  │─── PUBLISH (Packet ID) ───>│  Store → state=PUB_RECEIVED
  │                            │
  │<── PUBREC (Packet ID) ────│
  │                            │
  │  state=PUB_RELEASED        │
  │                            │
  │─── PUBREL (Packet ID) ────>│  Deliver → state=PUB_COMPLETED
  │                            │
  │<── PUBCOMP (Packet ID) ───│  Discard
  │                            │
  │  Discard                   │
```

服务端在接收客户端的 QoS 2 PUBLISH 时：
1. 收到 PUBLISH → 存储 Packet ID → 回复 PUBREC
2. 收到 PUBREL → 投递消息给订阅者 → 回复 PUBCOMP

服务端向客户端推送 QoS 2 消息时：
1. 发送 PUBLISH → 等待 PUBREC
2. 收到 PUBREC → 发送 PUBREL → 等待 PUBCOMP
3. 收到 PUBCOMP → 完成

## 6. MQTT 5.0 特性详细设计

### 6.1 Session Expiry Interval

- CONNECT 时 `Clean Start = true`：立即清除旧会话，新会话从零开始
- CONNECT 时 `Clean Start = false`：尝试恢复旧会话（订阅、QoS 2 状态）
- `Session Expiry Interval > 0`：断开后会话保留指定秒数
- `Session Expiry Interval = 0`：断开后立即清除会话
- DISCONNECT 报文可更新 Session Expiry Interval

### 6.2 Flow Control (Receive Maximum)

- CONNACK 中 `Receive Maximum` 告知客户端服务端可并发处理的 QoS 1/2 PUBLISH 数量
- CONNECT 中 `Receive Maximum` 告知服务端客户端可并发处理的数量
- 超出限制时，接收方可以断开连接（Reason Code 0x93）

### 6.3 Topic Alias

- 客户端/服务端通过 `Topic Alias Maximum` 协商支持的别名数量
- PUBLISH 时 `Topic Alias > 0` 且 `Topic` 非空：注册别名
- PUBLISH 时 `Topic Alias > 0` 且 `Topic` 为空：使用之前注册的别名对应的主题

### 6.4 Shared Subscription

- 格式：`$share/<group>/<topic_filter>`
- 同组内只有其中一个订阅者收到消息（负载均衡）
- 支持轮询（Round-Robin）策略

### 6.5 Subscription Identifier

- SUBSCRIBE 时可携带 `Subscription Identifier`
- 投递 PUBLISH 给订阅者时，在 PUBLISH 的 Properties 中携带匹配的 Subscription Identifier
- 允许订阅者区分消息来自哪个订阅

### 6.6 Request/Response

- PUBLISH 可携带 `Response Topic` 和 `Correlation Data`
- 接收方可在 `Response Topic` 上发布响应，附带 `Correlation Data`
- 协议层透传，不做特殊处理

### 6.7 Will Delay Interval

- 正常 DISCONNECT 不发送遗嘱消息
- 异常断开时，延迟 `Will Delay Interval` 秒后发送遗嘱消息
- 如果在延迟期间客户端重连并恢复会话，则取消遗嘱发送

### 6.8 Message Expiry Interval

- PUBLISH 携带 `Message Expiry Interval`（秒）
- 保留消息存储时记录过期时间
- 投递时检查是否过期，过期则丢弃
- 投递时 PUBLISH 的 `Message Expiry Interval` 更新为剩余秒数

## 7. 安全设计

### 7.1 认证

- **v3.1.1**: CONNECT 中的 username/password 明文认证
- **v5.0 增强认证**: AUTH 报文 + `Authentication Method` + `Authentication Data` 多轮交互
- 通过 `MqttHandler::on_connect` 和 `MqttHandler::on_auth` 回调由业务层实现

### 7.2 TLS

- 通过 `AsyncListenerHost::set_ssl_module()` 启用
- 可同时监听 1883（明文）和 8883（TLS）端口

### 7.3 主题授权

- 通过 `MqttHandler::on_subscribe` 和 `MqttHandler::on_publish` 回调控制
- 业务层可返回 false 拒绝订阅/发布

## 8. 性能设计

### 8.1 主题匹配

- 使用树结构 + 通配符节点，避免全量遍历
- 对于 `sensor/+/temperature` 类订阅，`+` 节点只需一次匹配
- 匹配时间复杂度：O(T * D)，T 为主题层级数，D 为通配符分支数

### 8.2 保留消息

- 使用 `std::map<topic, message>` 直接查找
- 订阅时的保留消息匹配复用主题树的匹配逻辑

### 8.3 连接管理

- 异步非阻塞 + 协程，单线程处理数千连接
- Keep Alive 检测通过 EventLoop 定时器

### 8.4 内存管理

- ByteBuffer 的 compact() 机制自动回收已读缓冲区
- 保留消息和会话状态使用智能指针管理

## 9. 错误处理

| 场景 | 处理 |
|------|------|
| 协议格式错误 | 关闭连接（MQTT 5.0: DISCONNECT Reason 0x81） |
| 非法报文类型 | 关闭连接（v3.1.1 / v5.0 规范要求） |
| Packet ID 冲突 | 返回对应 ACK 带 Reason Code 0x90 |
| 认证失败 | CONNACK Reason 0x86 / 0x87，关闭连接 |
| Keep Alive 超时 | 关闭连接，发送遗嘱消息 |
| 报文过大 | MQTT 5.0: DISCONNECT Reason 0x95 |
| 流控超限 | MQTT 5.0: DISCONNECT Reason 0x93 |

## 10. 测试策略

### 10.1 单元测试

- MqttCodec 编解码正确性
- Variable Byte Integer 编解码
- Properties 编解码
- MqttTopicTree 主题匹配（含通配符）
- MqttRetainedStore 保留消息存取
- MqttSession 状态管理
- QoS 2 状态机

### 10.2 集成测试

- 使用 MQTT 客户端（mosquitto_pub/sub、paho-mqtt）连接测试
- QoS 0/1/2 消息投递
- 遗嘱消息触发
- 保留消息存取
- 通配符订阅
- MQTT 5.0 特性（Properties、Topic Alias、Flow Control）
- TLS 连接

## 11. 文件清单

| # | 文件路径 | 说明 |
|---|---------|------|
| 1 | `docs/protocols/MQTT_DESIGN.md` | 本设计文档 |
| 2 | `protocol/mqtt/CMakeLists.txt` | 构建配置 |
| 3 | `protocol/mqtt/include/mqtt.h` | 统一头文件 |
| 4 | `protocol/mqtt/include/mqtt_protocol.h` | 常量、枚举、Reason Code |
| 5 | `protocol/mqtt/include/mqtt_packet.h` | 报文结构体 |
| 6 | `protocol/mqtt/include/mqtt_properties.h` | MQTT 5.0 Properties |
| 7 | `protocol/mqtt/include/mqtt_codec.h` | 编解码器 |
| 8 | `protocol/mqtt/include/mqtt_config.h` | 服务器配置 |
| 9 | `protocol/mqtt/include/mqtt_handler.h` | 业务回调接口 |
| 10 | `protocol/mqtt/include/mqtt_session.h` | 会话管理 |
| 11 | `protocol/mqtt/include/mqtt_topic_tree.h` | 主题订阅树 |
| 12 | `protocol/mqtt/include/mqtt_retained_store.h` | 保留消息存储 |
| 13 | `protocol/mqtt/include/mqtt_dispatcher.h` | 命令分发 |
| 14 | `protocol/mqtt/include/mqtt_server.h` | 服务器核心 |
| 15 | `protocol/mqtt/src/mqtt_codec.cpp` | 编解码实现 |
| 16 | `protocol/mqtt/src/mqtt_properties.cpp` | Properties 实现 |
| 17 | `protocol/mqtt/src/mqtt_session.cpp` | 会话实现 |
| 18 | `protocol/mqtt/src/mqtt_topic_tree.cpp` | 主题树实现 |
| 19 | `protocol/mqtt/src/mqtt_retained_store.cpp` | 保留消息实现 |
| 20 | `protocol/mqtt/src/mqtt_dispatcher.cpp` | 命令处理实现 |
| 21 | `protocol/mqtt/src/mqtt_server.cpp` | 服务器实现 |
| 22 | `server/services/include/mqtt_service.h` | 服务集成头文件 |
| 23 | `server/services/src/mqtt_service.cpp` | 服务集成实现 |
| 24 | `test/test_mqtt.cpp` | 单元测试 |
