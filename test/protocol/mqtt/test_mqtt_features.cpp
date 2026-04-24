#include "mqtt.h"
#include "mqtt_service.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

using namespace yuan::net::mqtt;

namespace
{
    std::atomic<int> g_failed{ 0 };

    struct WillOptions
    {
        std::string topic;
        std::vector<uint8_t> payload;
        QoS qos = QoS::AT_MOST_ONCE;
        bool retain = false;
    };

    void check(bool cond, const char * msg)
    {
        if (!cond) {
            ++g_failed;
            std::cerr << "[FAIL] " << msg << '\n';
        }
    }

    void close_socket(socket_t s)
    {
        if (s == kInvalidSocket)
            return;
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
    }

    bool send_all(socket_t s, const std::vector<uint8_t> & data)
    {
        size_t sent = 0;
        while (sent < data.size()) {
#ifdef _WIN32
            const int rc = ::send(s,
                                  reinterpret_cast<const char *>(data.data() + sent),
                                  static_cast<int>(data.size() - sent), 0);
#else
            const ssize_t rc = ::send(s, data.data() + sent, data.size() - sent, 0);
#endif
            if (rc <= 0)
                return false;
            sent += static_cast<size_t>(rc);
        }
        return true;
    }

    bool recv_exact(socket_t s, uint8_t * dst, size_t n)
    {
        size_t off = 0;
        while (off < n) {
#ifdef _WIN32
            const int rc = ::recv(s, reinterpret_cast<char *>(dst + off), static_cast<int>(n - off), 0);
#else
            const ssize_t rc = ::recv(s, dst + off, n - off, 0);
#endif
            if (rc <= 0)
                return false;
            off += static_cast<size_t>(rc);
        }
        return true;
    }

    std::optional<std::vector<uint8_t> > recv_one_packet(socket_t s)
    {
        uint8_t header[2] = { 0, 0 };
        if (!recv_exact(s, header, 2))
            return std::nullopt;

        std::vector<uint8_t> packet;
        packet.push_back(header[0]);
        packet.push_back(header[1]);

        uint32_t remaining = static_cast<uint32_t>(header[1] & 0x7F);
        uint32_t multiplier = 128;
        size_t vbi_extra = 0;
        while ((packet[1 + vbi_extra] & 0x80) != 0) {
            if (vbi_extra >= 3)
                return std::nullopt;
            uint8_t b = 0;
            if (!recv_exact(s, &b, 1))
                return std::nullopt;
            packet.push_back(b);
            ++vbi_extra;
            remaining += static_cast<uint32_t>(b & 0x7F) * multiplier;
            multiplier *= 128;
        }

        if (remaining > 0) {
            std::vector<uint8_t> body(remaining);
            if (!recv_exact(s, body.data(), body.size()))
                return std::nullopt;
            packet.insert(packet.end(), body.begin(), body.end());
        }

        return packet;
    }

    socket_t connect_loopback(uint16_t port)
    {
        socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s == kInvalidSocket)
            return kInvalidSocket;

#ifdef _WIN32
        const DWORD timeout_ms = 3000;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
        (void)::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                           reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
#else
        timeval tv{};
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::connect(s, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(s);
            return kInvalidSocket;
        }
        return s;
    }

    uint16_t reserve_tcp_port()
    {
        socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener == kInvalidSocket)
            return 0;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(listener);
            return 0;
        }

        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(listener, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            close_socket(listener);
            return 0;
        }

        const uint16_t port = ntohs(bound.sin_port);
        close_socket(listener);
        return port;
    }

    void append_utf8(yuan::buffer::ByteBuffer & body, const std::string & s)
    {
        body.append_u16(static_cast<uint16_t>(s.size()));
        if (!s.empty())
            body.append(s.data(), s.size());
    }

    std::vector<uint8_t> to_vector(const yuan::buffer::ByteBuffer & buf)
    {
        auto span = buf.readable_span();
        auto * begin = reinterpret_cast<const uint8_t *>(span.data());
        return std::vector<uint8_t>(begin, begin + span.size());
    }

    std::vector<uint8_t> build_connect_packet(ProtocolLevel level, const std::string & client_id,
                                              const std::optional<std::string> & username = std::nullopt,
                                              const std::optional<std::string> & password = std::nullopt,
                                              const std::optional<WillOptions> & will = std::nullopt)
    {
        yuan::buffer::ByteBuffer body(160);
        append_utf8(body, "MQTT");
        body.append_u8(static_cast<uint8_t>(level));

        uint8_t connect_flags = MQTT_CONNECT_FLAG_CLEAN_START;
        if (will.has_value()) {
            connect_flags |= MQTT_CONNECT_FLAG_WILL_FLAG;
            connect_flags |= (static_cast<uint8_t>(will->qos) << MQTT_CONNECT_FLAG_WILL_QOS_SHIFT);
            if (will->retain)
                connect_flags |= MQTT_CONNECT_FLAG_WILL_RETAIN;
        }
        if (username.has_value())
            connect_flags |= MQTT_CONNECT_FLAG_USERNAME;
        if (password.has_value())
            connect_flags |= MQTT_CONNECT_FLAG_PASSWORD;

        body.append_u8(connect_flags);
        body.append_u16(30);

        if (level == ProtocolLevel::V5_0) {
            encode_properties(body, MqttProperties{});
        }

        append_utf8(body, client_id);
        if (will.has_value()) {
            if (level == ProtocolLevel::V5_0)
                encode_properties(body, MqttProperties{});
            append_utf8(body, will->topic);
            body.append_u16(static_cast<uint16_t>(will->payload.size()));
            if (!will->payload.empty())
                body.append(will->payload.data(), will->payload.size());
        }
        if (username.has_value())
            append_utf8(body, *username);
        if (password.has_value())
            append_utf8(body, *password);

        auto packet = MqttCodec::build_fixed_header(PacketType::CONNECT, 0, body.write_offset());
        packet.append(body.readable_span());
        return to_vector(packet);
    }

    std::vector<uint8_t> build_connect_v311(const std::string & client_id)
    {
        return build_connect_packet(ProtocolLevel::V3_1_1, client_id);
    }

    std::vector<uint8_t> build_connect_v5(const std::string & client_id,
                                          const std::optional<std::string> & username = std::nullopt,
                                          const std::optional<std::string> & password = std::nullopt)
    {
        return build_connect_packet(ProtocolLevel::V5_0, client_id, username, password);
    }

    std::vector<uint8_t> build_subscribe_packet(ProtocolLevel level, uint16_t packet_id,
                                                const std::string & topic_filter, QoS qos)
    {
        yuan::buffer::ByteBuffer body(128);
        body.append_u16(packet_id);
        if (level == ProtocolLevel::V5_0)
            encode_properties(body, MqttProperties{});
        append_utf8(body, topic_filter);
        body.append_u8(static_cast<uint8_t>(qos) & 0x03);

        auto packet = MqttCodec::build_fixed_header(PacketType::SUBSCRIBE, 0x02, body.write_offset());
        packet.append(body.readable_span());
        return to_vector(packet);
    }

    std::vector<uint8_t> build_publish_packet(ProtocolLevel level,
                                              const std::string & topic,
                                              const std::vector<uint8_t> & payload,
                                              QoS qos,
                                              bool retain,
                                              std::optional<uint16_t> packet_id = std::nullopt,
                                              const std::optional<MqttProperties> & props = std::nullopt)
    {
        yuan::buffer::ByteBuffer body(256);
        append_utf8(body, topic);
        if (qos != QoS::AT_MOST_ONCE)
            body.append_u16(packet_id.value_or(1));
        if (level == ProtocolLevel::V5_0)
            encode_properties(body, props.value_or(MqttProperties{}));
        if (!payload.empty())
            body.append(payload.data(), payload.size());

        uint8_t flags = (static_cast<uint8_t>(qos) << MQTT_PUBLISH_FLAG_QOS_SHIFT) |
                        (retain ? MQTT_PUBLISH_FLAG_RETAIN : 0);
        auto packet = MqttCodec::build_fixed_header(PacketType::PUBLISH, flags, body.write_offset());
        packet.append(body.readable_span());
        return to_vector(packet);
    }

    std::vector<uint8_t> build_auth_packet_v5(const std::string & method,
                                              const std::vector<uint8_t> & auth_data = {})
    {
        yuan::buffer::ByteBuffer body(128);
        body.append_u8(static_cast<uint8_t>(AuthReason::CONTINUE_AUTHENTICATION));
        MqttProperties props;
        props.authentication_method = method;
        if (!auth_data.empty())
            props.authentication_data = auth_data;
        encode_properties(body, props);

        auto packet = MqttCodec::build_fixed_header(PacketType::AUTH, 0, body.write_offset());
        packet.append(body.readable_span());
        return to_vector(packet);
    }

    std::vector<uint8_t> build_pingreq_packet()
    {
        auto packet = MqttCodec::build_fixed_header(PacketType::PINGREQ, 0, 0);
        return to_vector(packet);
    }

    bool parse_packet_view(const std::vector<uint8_t> & packet,
                           PacketType & type,
                           uint8_t & flags,
                           const uint8_t *& body,
                           size_t & body_len);

    std::optional<uint8_t> connack_reason_code(const std::vector<uint8_t> & packet)
    {
        if (packet.size() < 4)
            return std::nullopt;
        if ((packet[0] >> 4) != static_cast<uint8_t>(PacketType::CONNACK))
            return std::nullopt;

        size_t index = 1;
        uint32_t remaining = 0;
        uint32_t multiplier = 1;
        for (size_t i = 0; i < MQTT_VARIABLE_BYTE_INT_MAX_SIZE && index < packet.size(); ++i) {
            uint8_t b = packet[index++];
            remaining += static_cast<uint32_t>(b & 0x7F) * multiplier;
            multiplier *= 128;
            if ((b & 0x80) == 0)
                break;
        }

        if (packet.size() < index + remaining)
            return std::nullopt;
        if (remaining < 2)
            return std::nullopt;
        return packet[index + 1];
    }

    std::optional<uint8_t> disconnect_reason_code_v5(const std::vector<uint8_t> & packet)
    {
        if (packet.size() < 3)
            return std::nullopt;
        if ((packet[0] >> 4) != static_cast<uint8_t>(PacketType::DISCONNECT))
            return std::nullopt;

        PacketType type = PacketType::CONNECT;
        uint8_t flags = 0;
        const uint8_t * body = nullptr;
        size_t body_len = 0;
        if (!parse_packet_view(packet, type, flags, body, body_len))
            return std::nullopt;
        if (body_len == 0)
            return static_cast<uint8_t>(DisconnectReason::NORMAL_DISCONNECTION);
        return body[0];
    }

    std::optional<uint8_t> suback_first_reason_code(const std::vector<uint8_t> & packet, ProtocolLevel level)
    {
        if (packet.empty())
            return std::nullopt;
        PacketType type = PacketType::CONNECT;
        uint8_t flags = 0;
        const uint8_t * body = nullptr;
        size_t body_len = 0;
        if (!parse_packet_view(packet, type, flags, body, body_len))
            return std::nullopt;
        if (type != PacketType::SUBACK || body_len < 3)
            return std::nullopt;

        size_t offset = 2;
        if (level == ProtocolLevel::V5_0) {
            MqttProperties props;
            if (decode_properties(body, body_len, offset, props) == 0)
                return std::nullopt;
        }

        if (offset >= body_len)
            return std::nullopt;
        return body[offset];
    }

    bool parse_packet_view(const std::vector<uint8_t> & packet,
                           PacketType & type,
                           uint8_t & flags,
                           const uint8_t *& body,
                           size_t & body_len)
    {
        if (packet.size() < 2)
            return false;
        flags = static_cast<uint8_t>(packet[0] & 0x0F);
        type = static_cast<PacketType>(packet[0] >> 4);

        size_t index = 1;
        uint32_t remaining = 0;
        uint32_t multiplier = 1;
        for (size_t i = 0; i < MQTT_VARIABLE_BYTE_INT_MAX_SIZE && index < packet.size(); ++i) {
            uint8_t b = packet[index++];
            remaining += static_cast<uint32_t>(b & 0x7F) * multiplier;
            multiplier *= 128;
            if ((b & 0x80) == 0)
                break;
        }

        if (packet.size() < index + remaining)
            return false;

        body = packet.data() + index;
        body_len = remaining;
        return true;
    }

    bool recv_until_type(socket_t s, PacketType want, std::vector<uint8_t> & out, int max_packets = 6)
    {
        for (int i = 0; i < max_packets; ++i) {
            auto packet = recv_one_packet(s);
            if (!packet.has_value())
                return false;
            if (packet->empty())
                continue;
            PacketType type = PacketType::CONNECT;
            uint8_t flags = 0;
            const uint8_t * body = nullptr;
            size_t body_len = 0;
            if (!parse_packet_view(*packet, type, flags, body, body_len))
                continue;
            if (type == want) {
                out = std::move(*packet);
                return true;
            }
        }
        return false;
    }

    class GateHandler final : public MqttHandler
    {
    public:
        bool allow_connect = true;
        std::optional<std::string> expected_username;
        std::optional<std::string> expected_password;
        std::string last_username;
        std::string last_password;
        int connect_calls = 0;

        bool on_connect(MqttSession *, const std::string &,
                        const std::string & username,
                        const std::string & password) override
        {
            ++connect_calls;
            last_username = username;
            last_password = password;

            if (!allow_connect)
                return false;
            if (expected_username.has_value() && username != *expected_username)
                return false;
            if (expected_password.has_value() && password != *expected_password)
                return false;
            return true;
        }
    };

    void test_connect_v311_ok(uint16_t port)
    {
        socket_t client = connect_loopback(port);
        check(client != kInvalidSocket, "v311: client should connect");
        if (client == kInvalidSocket)
            return;

        const auto connect = build_connect_v311("step3-v311");
        check(send_all(client, connect), "v311: send CONNECT should succeed");

        auto resp = recv_one_packet(client);
        check(resp.has_value(), "v311: should receive CONNACK");
        if (resp.has_value()) {
            auto rc = connack_reason_code(*resp);
            check(rc.has_value(), "v311: CONNACK reason should be parseable");
            if (rc.has_value()) {
                check(*rc == static_cast<uint8_t>(ConnackCode::ACCEPTED), "v311: CONNACK should be accepted");
            }
        }

        close_socket(client);
    }

    void test_connect_v5_ok_and_credentials(uint16_t port, GateHandler & handler)
    {
        handler.expected_username = std::string("u");
        handler.expected_password = std::string("p");

        socket_t client = connect_loopback(port);
        check(client != kInvalidSocket, "v5: client should connect");
        if (client == kInvalidSocket)
            return;

        const auto connect = build_connect_v5("step3-v5", std::string("u"), std::string("p"));
        check(send_all(client, connect), "v5: send CONNECT should succeed");

        auto resp = recv_one_packet(client);
        check(resp.has_value(), "v5: should receive CONNACK");
        if (resp.has_value()) {
            auto rc = connack_reason_code(*resp);
            check(rc.has_value(), "v5: CONNACK reason should be parseable");
            if (rc.has_value()) {
                check(*rc == static_cast<uint8_t>(ConnackCode::ACCEPTED), "v5: CONNACK should be accepted");
            }
        }

        check(handler.last_username == "u", "v5: handler should see username");
        check(handler.last_password == "p", "v5: handler should see password");

        handler.expected_username.reset();
        handler.expected_password.reset();
        close_socket(client);
    }

    void test_connect_not_authorized(uint16_t port, GateHandler & handler)
    {
        handler.allow_connect = false;

        socket_t client = connect_loopback(port);
        check(client != kInvalidSocket, "auth reject: client should connect");
        if (client == kInvalidSocket) {
            handler.allow_connect = true;
            return;
        }

        const auto connect = build_connect_v311("step3-auth-deny");
        check(send_all(client, connect), "auth reject: send CONNECT should succeed");

        auto resp = recv_one_packet(client);
        check(resp.has_value(), "auth reject: should receive CONNACK");
        if (resp.has_value()) {
            auto rc = connack_reason_code(*resp);
            check(rc.has_value(), "auth reject: reason should be parseable");
            if (rc.has_value()) {
                check(*rc == static_cast<uint8_t>(ConnackCode::NOT_AUTHORIZED), "auth reject: reason should be NOT_AUTHORIZED");
            }
        }

        handler.allow_connect = true;
        close_socket(client);
    }

    void test_duplicate_client_id_kicks_old_session(uint16_t port)
    {
        socket_t c1 = connect_loopback(port);
        check(c1 != kInvalidSocket, "dup client id: first client should connect");
        if (c1 == kInvalidSocket)
            return;

        const auto connect1 = build_connect_v311("dup-client");
        check(send_all(c1, connect1), "dup client id: first CONNECT should send");
        auto r1 = recv_one_packet(c1);
        check(r1.has_value(), "dup client id: first CONNACK should be received");

        socket_t c2 = connect_loopback(port);
        check(c2 != kInvalidSocket, "dup client id: second client should connect");
        if (c2 != kInvalidSocket) {
            const auto connect2 = build_connect_v311("dup-client");
            check(send_all(c2, connect2), "dup client id: second CONNECT should send");
            auto r2 = recv_one_packet(c2);
            check(r2.has_value(), "dup client id: second CONNACK should be received");
        }

        const auto ping = build_pingreq_packet();
        bool old_send_ok = send_all(c1, ping);
        auto old_resp = recv_one_packet(c1);
        check(!old_send_ok || !old_resp.has_value(), "dup client id: old connection should be closed");

        close_socket(c1);
        close_socket(c2);
    }

    void test_v5_unsupported_by_server_rejected()
    {
        const uint16_t port = reserve_tcp_port();
        check(port != 0, "unsupported version: should reserve port");
        if (port == 0)
            return;

        MqttServerConfig cfg;
        cfg.supported_versions = { ProtocolLevel::V3_1_1 };
        auto service = std::make_unique<yuan::server::MqttService>(port, cfg);
        const bool init_ok = service->init();
        check(init_ok, "unsupported version: service should init");
        if (!init_ok)
            return;

        service->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        socket_t client = connect_loopback(port);
        check(client != kInvalidSocket, "unsupported version: client should connect");
        if (client != kInvalidSocket) {
            const auto connect = build_connect_v5("step3-v5-unsupported");
            check(send_all(client, connect), "unsupported version: send CONNECT should succeed");
            auto resp = recv_one_packet(client);
            check(resp.has_value(), "unsupported version: should receive CONNACK");
            if (resp.has_value()) {
                auto rc = connack_reason_code(*resp);
                check(rc.has_value(), "unsupported version: reason should parse");
                if (rc.has_value()) {
                    check(*rc == static_cast<uint8_t>(ConnackCode::V5_UNSUPPORTED_PROTOCOL_VERSION),
                          "unsupported version: reason should be V5_UNSUPPORTED_PROTOCOL_VERSION");
                }
            }
        }

        close_socket(client);
        service->stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }

    void test_pubsub_qos0_delivery(uint16_t port)
    {
        socket_t sub = connect_loopback(port);
        check(sub != kInvalidSocket, "pubsub qos0: subscriber should connect");
        if (sub == kInvalidSocket)
            return;
        check(send_all(sub, build_connect_v311("step4-sub-qos0")), "pubsub qos0: subscriber CONNECT");
        std::vector<uint8_t> packet;
        check(recv_until_type(sub, PacketType::CONNACK, packet), "pubsub qos0: subscriber CONNACK");

        check(send_all(sub, build_subscribe_packet(ProtocolLevel::V3_1_1, 1, "step4/q0", QoS::AT_MOST_ONCE)),
              "pubsub qos0: SUBSCRIBE send");
        check(recv_until_type(sub, PacketType::SUBACK, packet), "pubsub qos0: SUBACK receive");

        socket_t pub = connect_loopback(port);
        check(pub != kInvalidSocket, "pubsub qos0: publisher should connect");
        if (pub != kInvalidSocket) {
            check(send_all(pub, build_connect_v311("step4-pub-qos0")), "pubsub qos0: publisher CONNECT");
            check(recv_until_type(pub, PacketType::CONNACK, packet), "pubsub qos0: publisher CONNACK");

            const std::vector<uint8_t> payload = { 'h', 'i', '0' };
            check(send_all(pub, build_publish_packet(ProtocolLevel::V3_1_1, "step4/q0", payload, QoS::AT_MOST_ONCE, false)),
                  "pubsub qos0: PUBLISH send");
            close_socket(pub);
        }

        std::vector<uint8_t> pub_packet;
        check(recv_until_type(sub, PacketType::PUBLISH, pub_packet), "pubsub qos0: subscriber should receive PUBLISH");
        if (!pub_packet.empty()) {
            PacketType type = PacketType::CONNECT;
            uint8_t flags = 0;
            const uint8_t * body = nullptr;
            size_t body_len = 0;
            if (parse_packet_view(pub_packet, type, flags, body, body_len)) {
                auto decoded = MqttCodec::decode_publish(body, body_len, flags, ProtocolLevel::V3_1_1);
                check(decoded.has_value(), "pubsub qos0: decode publish");
                if (decoded.has_value()) {
                    check(decoded->topic == "step4/q0", "pubsub qos0: topic should match");
                    check(decoded->payload == std::vector<uint8_t>({ 'h', 'i', '0' }), "pubsub qos0: payload should match");
                }
            }
        }

        close_socket(sub);
    }

    void test_publish_qos1_puback(uint16_t port)
    {
        socket_t client = connect_loopback(port);
        check(client != kInvalidSocket, "qos1: client should connect");
        if (client == kInvalidSocket)
            return;

        std::vector<uint8_t> packet;
        check(send_all(client, build_connect_v311("step4-qos1")), "qos1: CONNECT send");
        check(recv_until_type(client, PacketType::CONNACK, packet), "qos1: CONNACK receive");

        const uint16_t pid = 42;
        check(send_all(client, build_publish_packet(ProtocolLevel::V3_1_1, "step4/q1", { '1' }, QoS::AT_LEAST_ONCE, false, pid)),
              "qos1: PUBLISH send");

        check(recv_until_type(client, PacketType::PUBACK, packet), "qos1: PUBACK should be received");
        if (!packet.empty()) {
            PacketType type = PacketType::CONNECT;
            uint8_t flags = 0;
            const uint8_t * body = nullptr;
            size_t body_len = 0;
            if (parse_packet_view(packet, type, flags, body, body_len)) {
                auto ack = MqttCodec::decode_puback(body, body_len, ProtocolLevel::V3_1_1);
                check(ack.has_value(), "qos1: decode PUBACK");
                if (ack.has_value())
                    check(ack->packet_id == pid, "qos1: PUBACK packet id should match");
            }
        }

        close_socket(client);
    }

    void test_retain_delivery_on_subscribe(uint16_t port)
    {
        socket_t pub = connect_loopback(port);
        check(pub != kInvalidSocket, "retain: publisher should connect");
        if (pub == kInvalidSocket)
            return;

        std::vector<uint8_t> packet;
        check(send_all(pub, build_connect_v311("step4-retain-pub")), "retain: publisher CONNECT");
        check(recv_until_type(pub, PacketType::CONNACK, packet), "retain: publisher CONNACK");
        check(send_all(pub, build_publish_packet(ProtocolLevel::V3_1_1, "step4/retain", { 'R', 'T' }, QoS::AT_MOST_ONCE, true)),
              "retain: retained PUBLISH send");
        close_socket(pub);

        socket_t sub = connect_loopback(port);
        check(sub != kInvalidSocket, "retain: subscriber should connect");
        if (sub == kInvalidSocket)
            return;

        check(send_all(sub, build_connect_v311("step4-retain-sub")), "retain: subscriber CONNECT");
        check(recv_until_type(sub, PacketType::CONNACK, packet), "retain: subscriber CONNACK");
        check(send_all(sub, build_subscribe_packet(ProtocolLevel::V3_1_1, 2, "step4/retain", QoS::AT_MOST_ONCE)),
              "retain: SUBSCRIBE send");

        std::vector<uint8_t> retain_packet;
        check(recv_until_type(sub, PacketType::PUBLISH, retain_packet), "retain: should receive retained PUBLISH");
        if (!retain_packet.empty()) {
            PacketType type = PacketType::CONNECT;
            uint8_t flags = 0;
            const uint8_t * body = nullptr;
            size_t body_len = 0;
            if (parse_packet_view(retain_packet, type, flags, body, body_len)) {
                auto decoded = MqttCodec::decode_publish(body, body_len, flags, ProtocolLevel::V3_1_1);
                check(decoded.has_value(), "retain: decode retained publish");
                if (decoded.has_value()) {
                    check(decoded->retain == 1, "retain: retained message should carry retain flag");
                    check(decoded->topic == "step4/retain", "retain: topic should match");
                }
            }
        }

        close_socket(sub);
    }

    void test_will_message_on_ungraceful_disconnect(uint16_t port)
    {
        socket_t sub = connect_loopback(port);
        check(sub != kInvalidSocket, "will: subscriber should connect");
        if (sub == kInvalidSocket)
            return;

        std::vector<uint8_t> packet;
        check(send_all(sub, build_connect_v311("step4-will-sub")), "will: subscriber CONNECT");
        check(recv_until_type(sub, PacketType::CONNACK, packet), "will: subscriber CONNACK");
        check(send_all(sub, build_subscribe_packet(ProtocolLevel::V3_1_1, 3, "step4/will", QoS::AT_MOST_ONCE)),
              "will: SUBSCRIBE send");
        check(recv_until_type(sub, PacketType::SUBACK, packet), "will: SUBACK receive");

        WillOptions will;
        will.topic = "step4/will";
        will.payload = { 'B', 'Y', 'E' };
        will.qos = QoS::AT_MOST_ONCE;

        socket_t pub = connect_loopback(port);
        check(pub != kInvalidSocket, "will: publisher should connect");
        if (pub != kInvalidSocket) {
            check(send_all(pub, build_connect_packet(ProtocolLevel::V3_1_1, "step4-will-pub", std::nullopt, std::nullopt, will)),
                  "will: publisher CONNECT with will");
            check(recv_until_type(pub, PacketType::CONNACK, packet), "will: publisher CONNACK");
            close_socket(pub);
        }

        std::vector<uint8_t> will_packet;
        check(recv_until_type(sub, PacketType::PUBLISH, will_packet), "will: subscriber should receive will publish");
        if (!will_packet.empty()) {
            PacketType type = PacketType::CONNECT;
            uint8_t flags = 0;
            const uint8_t * body = nullptr;
            size_t body_len = 0;
            if (parse_packet_view(will_packet, type, flags, body, body_len)) {
                auto decoded = MqttCodec::decode_publish(body, body_len, flags, ProtocolLevel::V3_1_1);
                check(decoded.has_value(), "will: decode publish");
                if (decoded.has_value()) {
                    check(decoded->topic == "step4/will", "will: topic should match");
                    check(decoded->payload == std::vector<uint8_t>({ 'B', 'Y', 'E' }), "will: payload should match");
                }
            }
        }

        close_socket(sub);
    }

    void test_v5_topic_alias_publish(uint16_t port)
    {
        socket_t sub = connect_loopback(port);
        check(sub != kInvalidSocket, "alias: subscriber should connect");
        if (sub == kInvalidSocket)
            return;

        std::vector<uint8_t> packet;
        check(send_all(sub, build_connect_v311("step4-alias-sub")), "alias: subscriber CONNECT");
        check(recv_until_type(sub, PacketType::CONNACK, packet), "alias: subscriber CONNACK");
        check(send_all(sub, build_subscribe_packet(ProtocolLevel::V3_1_1, 4, "step4/alias", QoS::AT_MOST_ONCE)),
              "alias: SUBSCRIBE send");
        check(recv_until_type(sub, PacketType::SUBACK, packet), "alias: SUBACK receive");

        socket_t pub = connect_loopback(port);
        check(pub != kInvalidSocket, "alias: publisher should connect");
        if (pub != kInvalidSocket) {
            check(send_all(pub, build_connect_v5("step4-alias-pub")), "alias: publisher CONNECT v5");
            check(recv_until_type(pub, PacketType::CONNACK, packet), "alias: publisher CONNACK");

            MqttProperties p1;
            p1.topic_alias = 3;
            check(send_all(pub, build_publish_packet(ProtocolLevel::V5_0, "step4/alias", { 'A', '1' }, QoS::AT_MOST_ONCE, false, std::nullopt, p1)),
                  "alias: first publish with topic alias");

            MqttProperties p2;
            p2.topic_alias = 3;
            check(send_all(pub, build_publish_packet(ProtocolLevel::V5_0, "", { 'A', '2' }, QoS::AT_MOST_ONCE, false, std::nullopt, p2)),
                  "alias: second publish alias-only");

            close_socket(pub);
        }

        std::vector<uint8_t> pub1;
        std::vector<uint8_t> pub2;
        check(recv_until_type(sub, PacketType::PUBLISH, pub1), "alias: should receive first publish");
        check(recv_until_type(sub, PacketType::PUBLISH, pub2), "alias: should receive second publish");

        auto verify_pub = [&](const std::vector<uint8_t> & pkt, const std::vector<uint8_t> & want_payload, const char * msg_prefix) {
            if (pkt.empty())
                return;
            PacketType type = PacketType::CONNECT;
            uint8_t flags = 0;
            const uint8_t * body = nullptr;
            size_t body_len = 0;
            if (!parse_packet_view(pkt, type, flags, body, body_len))
                return;
            auto decoded = MqttCodec::decode_publish(body, body_len, flags, ProtocolLevel::V3_1_1);
            check(decoded.has_value(), msg_prefix);
            if (decoded.has_value()) {
                check(decoded->topic == "step4/alias", "alias: resolved topic should be step4/alias");
                check(decoded->payload == want_payload, "alias: payload should match expected");
            }
        };

        verify_pub(pub1, { 'A', '1' }, "alias: decode first publish");
        verify_pub(pub2, { 'A', '2' }, "alias: decode second publish");

        close_socket(sub);
    }

    void test_packet_before_connect_closes_connection(uint16_t port)
    {
        socket_t client = connect_loopback(port);
        check(client != kInvalidSocket, "pre-connect packet: client should connect");
        if (client == kInvalidSocket)
            return;

        check(send_all(client, build_pingreq_packet()), "pre-connect packet: send PINGREQ");
        auto resp = recv_one_packet(client);
        check(!resp.has_value(), "pre-connect packet: server should close without response");
        close_socket(client);
    }

    void test_v5_auth_rejected_by_default_handler(uint16_t port)
    {
        socket_t client = connect_loopback(port);
        check(client != kInvalidSocket, "auth reject: client should connect");
        if (client == kInvalidSocket)
            return;

        std::vector<uint8_t> packet;
        check(send_all(client, build_connect_v5("step5-auth-v5")), "auth reject: CONNECT v5 send");
        check(recv_until_type(client, PacketType::CONNACK, packet), "auth reject: CONNACK receive");

        check(send_all(client, build_auth_packet_v5("scram", { 1, 2, 3 })), "auth reject: AUTH send");
        check(recv_until_type(client, PacketType::DISCONNECT, packet), "auth reject: should receive DISCONNECT");
        if (!packet.empty()) {
            auto reason = disconnect_reason_code_v5(packet);
            check(reason.has_value(), "auth reject: DISCONNECT reason should parse");
            if (reason.has_value()) {
                check(*reason == static_cast<uint8_t>(ConnackCode::V5_BAD_AUTHENTICATION_METHOD),
                      "auth reject: reason should be V5_BAD_AUTHENTICATION_METHOD");
            }
        }

        close_socket(client);
    }

    void test_v5_invalid_subscribe_filter_reason(uint16_t port)
    {
        socket_t client = connect_loopback(port);
        check(client != kInvalidSocket, "invalid subscribe: client should connect");
        if (client == kInvalidSocket)
            return;

        std::vector<uint8_t> packet;
        check(send_all(client, build_connect_v5("step5-invalid-sub")), "invalid subscribe: CONNECT v5 send");
        check(recv_until_type(client, PacketType::CONNACK, packet), "invalid subscribe: CONNACK receive");

        check(send_all(client, build_subscribe_packet(ProtocolLevel::V5_0, 9, "bad/#/tail", QoS::AT_LEAST_ONCE)),
              "invalid subscribe: SUBSCRIBE send");
        check(recv_until_type(client, PacketType::SUBACK, packet), "invalid subscribe: SUBACK receive");

        auto reason = suback_first_reason_code(packet, ProtocolLevel::V5_0);
        check(reason.has_value(), "invalid subscribe: SUBACK reason should parse");
        if (reason.has_value()) {
            check(*reason == static_cast<uint8_t>(SubackReason::TOPIC_FILTER_INVALID),
                  "invalid subscribe: reason should be TOPIC_FILTER_INVALID");
        }

        close_socket(client);
    }

    void test_v5_packet_too_large_disconnect()
    {
        const uint16_t port = reserve_tcp_port();
        check(port != 0, "packet too large: should reserve port");
        if (port == 0)
            return;

        MqttServerConfig cfg;
        cfg.max_packet_size = 64;
        auto service = std::make_unique<yuan::server::MqttService>(port, cfg);
        const bool init_ok = service->init();
        check(init_ok, "packet too large: service should init");
        if (!init_ok)
            return;

        service->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        socket_t client = connect_loopback(port);
        check(client != kInvalidSocket, "packet too large: client should connect");
        if (client != kInvalidSocket) {
            std::vector<uint8_t> packet;
            check(send_all(client, build_connect_v5("step5-large")), "packet too large: CONNECT v5 send");
            check(recv_until_type(client, PacketType::CONNACK, packet), "packet too large: CONNACK receive");

            std::vector<uint8_t> huge_payload(160, static_cast<uint8_t>('Z'));
            check(send_all(client, build_publish_packet(ProtocolLevel::V5_0, "step5/large", huge_payload, QoS::AT_MOST_ONCE, false)),
                  "packet too large: huge PUBLISH send");

            check(recv_until_type(client, PacketType::DISCONNECT, packet), "packet too large: should receive DISCONNECT");
            if (!packet.empty()) {
                auto reason = disconnect_reason_code_v5(packet);
                check(reason.has_value(), "packet too large: reason should parse");
                if (reason.has_value()) {
                    check(*reason == static_cast<uint8_t>(DisconnectReason::PACKET_TOO_LARGE),
                          "packet too large: reason should be PACKET_TOO_LARGE");
                }
            }
        }

        close_socket(client);
        service->stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}

int main()
{
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    const uint16_t port = reserve_tcp_port();
    check(port != 0, "should reserve mqtt test port");
    if (port == 0) {
        return 1;
    }

    MqttServerConfig cfg;
    cfg.idle_timeout_ms = 3000;
    auto service = std::make_unique<yuan::server::MqttService>(port, cfg);
    GateHandler handler;
    service->set_handler(&handler);
    const bool init_ok = service->init();
    check(init_ok, "mqtt service should init");
    if (!init_ok) {
        return 1;
    }

    service->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    test_connect_v311_ok(port);
    test_connect_v5_ok_and_credentials(port, handler);
    test_connect_not_authorized(port, handler);
    test_duplicate_client_id_kicks_old_session(port);
    test_pubsub_qos0_delivery(port);
    test_publish_qos1_puback(port);
    test_retain_delivery_on_subscribe(port);
    test_will_message_on_ungraceful_disconnect(port);
    test_v5_topic_alias_publish(port);
    test_packet_before_connect_closes_connection(port);
    test_v5_auth_rejected_by_default_handler(port);
    test_v5_invalid_subscribe_filter_reason(port);

    service->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    test_v5_unsupported_by_server_rejected();
    test_v5_packet_too_large_disconnect();

    const int exit_code = g_failed.load() > 0 ? 1 : 0;
    if (exit_code == 0)
        std::cout << "mqtt feature step5 passed\n";
    else
        std::cerr << "mqtt feature skeleton failed=" << g_failed.load() << '\n';

#ifdef _WIN32
    WSACleanup();
    ::ExitProcess(static_cast<UINT>(exit_code));
#endif

    return exit_code;
}
    struct WillOptions
    {
        std::string topic;
        std::vector<uint8_t> payload;
        QoS qos = QoS::AT_MOST_ONCE;
        bool retain = false;
    };
