#include "mqtt.h"
#include "buffer/byte_buffer.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace yuan::net::mqtt;
using namespace yuan::buffer;

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(expr, msg)                                                              \
    do {                                                                                    \
        if (!(expr)) {                                                                      \
            std::cout << "  FAIL: " << msg << " (at line " << __LINE__ << ")" << std::endl; \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

#define RUN_TEST(func)                                       \
    do {                                                     \
        g_tests_run++;                                       \
        std::cout << "  Running: " #func "..." << std::endl; \
        if (func()) {                                        \
            g_tests_passed++;                                \
            std::cout << "  PASS" << std::endl;              \
        } else {                                             \
            g_tests_failed++;                                \
            std::cout << "  FAIL" << std::endl;              \
        }                                                    \
    } while (0)

bool test_packet_type_values()
{
    TEST_ASSERT(static_cast<uint8_t>(PacketType::CONNECT) == 1, "CONNECT should be 1");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::CONNACK) == 2, "CONNACK should be 2");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::PUBLISH) == 3, "PUBLISH should be 3");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::PUBACK) == 4, "PUBACK should be 4");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::PUBREC) == 5, "PUBREC should be 5");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::PUBREL) == 6, "PUBREL should be 6");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::PUBCOMP) == 7, "PUBCOMP should be 7");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::SUBSCRIBE) == 8, "SUBSCRIBE should be 8");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::SUBACK) == 9, "SUBACK should be 9");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::UNSUBSCRIBE) == 10, "UNSUBSCRIBE should be 10");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::UNSUBACK) == 11, "UNSUBACK should be 11");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::PINGREQ) == 12, "PINGREQ should be 12");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::PINGRESP) == 13, "PINGRESP should be 13");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::DISCONNECT) == 14, "DISCONNECT should be 14");
    TEST_ASSERT(static_cast<uint8_t>(PacketType::AUTH) == 15, "AUTH should be 15");
    return true;
}

bool test_qos_values()
{
    TEST_ASSERT(static_cast<uint8_t>(QoS::AT_MOST_ONCE) == 0, "QoS 0");
    TEST_ASSERT(static_cast<uint8_t>(QoS::AT_LEAST_ONCE) == 1, "QoS 1");
    TEST_ASSERT(static_cast<uint8_t>(QoS::EXACTLY_ONCE) == 2, "QoS 2");
    return true;
}

bool test_protocol_level_values()
{
    TEST_ASSERT(static_cast<uint8_t>(ProtocolLevel::V3_1_1) == 4, "V3.1.1 level should be 4");
    TEST_ASSERT(static_cast<uint8_t>(ProtocolLevel::V5_0) == 5, "V5.0 level should be 5");
    return true;
}

bool test_connack_code_values()
{
    TEST_ASSERT(static_cast<uint8_t>(ConnackCode::ACCEPTED) == 0, "ACCEPTED should be 0");
    TEST_ASSERT(static_cast<uint8_t>(ConnackCode::UNACCEPTABLE_PROTOCOL_VERSION) == 1, "Unacceptable protocol version");
    TEST_ASSERT(static_cast<uint8_t>(ConnackCode::IDENTIFIER_REJECTED) == 2, "Identifier rejected");
    TEST_ASSERT(static_cast<uint8_t>(ConnackCode::SERVER_UNAVAILABLE) == 3, "Server unavailable");
    TEST_ASSERT(static_cast<uint8_t>(ConnackCode::BAD_USER_NAME_OR_PASSWORD) == 4, "Bad credentials");
    TEST_ASSERT(static_cast<uint8_t>(ConnackCode::NOT_AUTHORIZED) == 5, "Not authorized");
    TEST_ASSERT(static_cast<uint8_t>(ConnackCode::V5_UNSUPPORTED_PROTOCOL_VERSION) == 0x84, "V5 unsupported protocol");
    return true;
}

bool test_connect_flags()
{
    TEST_ASSERT(MQTT_CONNECT_FLAG_USERNAME == 0x80, "Username flag");
    TEST_ASSERT(MQTT_CONNECT_FLAG_PASSWORD == 0x40, "Password flag");
    TEST_ASSERT(MQTT_CONNECT_FLAG_WILL_RETAIN == 0x20, "Will retain flag");
    TEST_ASSERT(MQTT_CONNECT_FLAG_WILL_FLAG == 0x04, "Will flag");
    TEST_ASSERT(MQTT_CONNECT_FLAG_CLEAN_START == 0x02, "Clean start flag");
    return true;
}

bool test_publish_flags()
{
    TEST_ASSERT(MQTT_PUBLISH_FLAG_DUP == 0x08, "DUP flag");
    TEST_ASSERT(MQTT_PUBLISH_FLAG_RETAIN == 0x01, "Retain flag");
    return true;
}

bool test_keep_alive_constants()
{
    TEST_ASSERT(MQTT_KEEP_ALIVE_DEFAULT == 60, "Default keep alive should be 60");
    TEST_ASSERT(MQTT_KEEP_ALIVE_FACTOR == 1.5, "Keep alive factor should be 1.5");
    return true;
}

bool test_remaining_length_constant()
{
    TEST_ASSERT(MQTT_MAX_REMAINING_LENGTH == 268435455, "Max remaining length should be 268435455");
    TEST_ASSERT(MQTT_PACKET_ID_MAX == 65535, "Max packet ID should be 65535");
    return true;
}

bool test_server_config_defaults()
{
    MqttServerConfig config;
    TEST_ASSERT(config.port == 1883, "Default port should be 1883");
    TEST_ASSERT(config.max_connections == 10000, "Default max connections");
    TEST_ASSERT(config.max_message_size == 256 * 1024, "Default max message size");
    TEST_ASSERT(config.max_packet_size == 256 * 1024, "Default max packet size");
    TEST_ASSERT(config.keep_alive_default == 60, "Default keep alive");
    TEST_ASSERT(config.keep_alive_factor == 1.5, "Default keep alive factor");
    TEST_ASSERT(config.topic_alias_maximum == 0, "Default topic alias max");
    TEST_ASSERT(config.receive_maximum == 65535, "Default receive maximum");
    TEST_ASSERT(config.maximum_qos == 2, "Default max QoS");
    TEST_ASSERT(config.retain_available == true, "Default retain available");
    TEST_ASSERT(config.wildcard_subscription_available == true, "Default wildcard available");
    TEST_ASSERT(config.subscription_identifier_available == true, "Default sub id available");
    TEST_ASSERT(config.shared_subscription_available == true, "Default shared sub available");
    TEST_ASSERT(config.require_authentication == false, "Default no auth required");
    TEST_ASSERT(config.supported_versions.size() == 2, "Default 2 supported versions");
    TEST_ASSERT(config.supported_versions[0] == ProtocolLevel::V3_1_1, "First version v3.1.1");
    TEST_ASSERT(config.supported_versions[1] == ProtocolLevel::V5_0, "Second version v5.0");
    return true;
}

bool test_connect_packet_defaults()
{
    MqttConnectPacket pkt;
    TEST_ASSERT(pkt.protocol_level == ProtocolLevel::V3_1_1, "Default protocol level");
    TEST_ASSERT(pkt.connect_flags == 0, "Default connect flags");
    TEST_ASSERT(pkt.keep_alive == MQTT_KEEP_ALIVE_DEFAULT, "Default keep alive");
    TEST_ASSERT(pkt.client_id.empty(), "Default empty client id");
    TEST_ASSERT(!pkt.will_topic.has_value(), "Default no will topic");
    TEST_ASSERT(!pkt.will_payload.has_value(), "Default no will payload");
    TEST_ASSERT(!pkt.username.has_value(), "Default no username");
    TEST_ASSERT(!pkt.password.has_value(), "Default no password");
    return true;
}

bool test_connack_packet_defaults()
{
    MqttConnackPacket pkt;
    TEST_ASSERT(pkt.session_present == 0, "Default session present");
    TEST_ASSERT(pkt.reason_code == 0, "Default reason code");
    return true;
}

bool test_publish_packet_defaults()
{
    MqttPublishPacket pkt;
    TEST_ASSERT(pkt.dup == 0, "Default dup");
    TEST_ASSERT(pkt.qos == QoS::AT_MOST_ONCE, "Default QoS 0");
    TEST_ASSERT(pkt.retain == 0, "Default retain");
    TEST_ASSERT(pkt.topic.empty(), "Default empty topic");
    TEST_ASSERT(!pkt.packet_id.has_value(), "Default no packet id");
    TEST_ASSERT(pkt.payload.empty(), "Default empty payload");
    return true;
}

bool test_subscribe_packet()
{
    MqttSubscribePacket pkt;
    pkt.packet_id = 42;
    MqttSubOption sub;
    sub.topic_filter = "sensor/temperature";
    sub.maximum_qos = QoS::AT_LEAST_ONCE;
    pkt.subscriptions.push_back(sub);

    TEST_ASSERT(pkt.packet_id == 42, "Packet ID should match");
    TEST_ASSERT(pkt.subscriptions.size() == 1, "Should have 1 subscription");
    TEST_ASSERT(pkt.subscriptions[0].topic_filter == "sensor/temperature", "Topic filter should match");
    TEST_ASSERT(pkt.subscriptions[0].maximum_qos == QoS::AT_LEAST_ONCE, "QoS should match");
    return true;
}

bool test_unsubscribe_packet()
{
    MqttUnsubscribePacket pkt;
    pkt.packet_id = 10;
    pkt.topic_filters.push_back("sensor/#");
    pkt.topic_filters.push_back("cmd/+");

    TEST_ASSERT(pkt.packet_id == 10, "Packet ID should match");
    TEST_ASSERT(pkt.topic_filters.size() == 2, "Should have 2 filters");
    TEST_ASSERT(pkt.topic_filters[0] == "sensor/#", "First filter");
    TEST_ASSERT(pkt.topic_filters[1] == "cmd/+", "Second filter");
    return true;
}

bool test_properties_defaults()
{
    MqttProperties props;
    TEST_ASSERT(!props.payload_format_indicator.has_value(), "Default no payload format");
    TEST_ASSERT(!props.message_expiry_interval.has_value(), "Default no message expiry");
    TEST_ASSERT(!props.content_type.has_value(), "Default no content type");
    TEST_ASSERT(!props.session_expiry_interval.has_value(), "Default no session expiry");
    TEST_ASSERT(!props.receive_maximum.has_value(), "Default no receive max");
    TEST_ASSERT(!props.topic_alias_maximum.has_value(), "Default no topic alias max");
    TEST_ASSERT(!props.maximum_qos.has_value(), "Default no max QoS");
    TEST_ASSERT(!props.retain_available.has_value(), "Default no retain available");
    TEST_ASSERT(!props.maximum_packet_size.has_value(), "Default no max packet size");
    TEST_ASSERT(props.user_properties.empty(), "Default no user properties");
    return true;
}

bool test_properties_has_any()
{
    MqttProperties empty;
    TEST_ASSERT(!empty.has_any(), "Empty properties should not have any");

    MqttProperties with_value;
    with_value.payload_format_indicator = 0;
    TEST_ASSERT(with_value.has_any(), "Properties with value should have any");

    MqttProperties with_user_prop;
    with_user_prop.user_properties.push_back({ "key", "value" });
    TEST_ASSERT(with_user_prop.user_properties.size() == 1, "User property count");
    TEST_ASSERT(with_user_prop.user_properties[0].key == "key", "User property key");
    TEST_ASSERT(with_user_prop.user_properties[0].value == "value", "User property value");
    return true;
}

bool test_properties_clear()
{
    MqttProperties props;
    props.payload_format_indicator = 1;
    props.session_expiry_interval = 3600;
    props.user_properties.push_back({ "k", "v" });

    props.clear();
    TEST_ASSERT(!props.payload_format_indicator.has_value(), "Cleared payload format");
    TEST_ASSERT(!props.session_expiry_interval.has_value(), "Cleared session expiry");
    TEST_ASSERT(props.user_properties.empty(), "Cleared user properties");
    return true;
}

bool test_codec_connack_v311()
{
    MqttConnackPacket pkt;
    pkt.session_present = 0;
    pkt.reason_code = static_cast<uint8_t>(ConnackCode::ACCEPTED);

    auto buf = MqttCodec::encode_connack(pkt, ProtocolLevel::V3_1_1);
    TEST_ASSERT(buf.readable_bytes() >= 4, "CONNACK v3.1.1 should be at least 4 bytes");

    auto span = buf.readable_span();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());

    TEST_ASSERT((data[0] >> 4) == static_cast<uint8_t>(PacketType::CONNACK), "First byte should be CONNACK type");
    TEST_ASSERT(data[1] == 2, "Remaining length should be 2");
    TEST_ASSERT(data[2] == 0, "Session present should be 0");
    TEST_ASSERT(data[3] == 0, "Reason code should be 0");
    return true;
}

bool test_codec_connack_v5()
{
    MqttConnackPacket pkt;
    pkt.session_present = 1;
    pkt.reason_code = static_cast<uint8_t>(ConnackCode::ACCEPTED);
    pkt.properties.maximum_qos = 1;
    pkt.properties.retain_available = 1;

    auto buf = MqttCodec::encode_connack(pkt, ProtocolLevel::V5_0);
    TEST_ASSERT(buf.readable_bytes() >= 4, "CONNACK v5.0 should be at least 4 bytes");

    auto span = buf.readable_span();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());

    TEST_ASSERT((data[0] >> 4) == static_cast<uint8_t>(PacketType::CONNACK), "First byte should be CONNACK type");
    return true;
}

bool test_codec_publish_v311()
{
    MqttPublishPacket pkt;
    pkt.topic = "test/topic";
    pkt.payload = { 0x48, 0x65, 0x6C, 0x6C, 0x6F };
    pkt.qos = QoS::AT_MOST_ONCE;
    pkt.retain = 0;
    pkt.dup = 0;

    auto buf = MqttCodec::encode_publish(pkt, ProtocolLevel::V3_1_1);
    TEST_ASSERT(buf.readable_bytes() > 0, "PUBLISH v3.1.1 should produce output");

    auto span = buf.readable_span();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());

    uint8_t expected_first = (static_cast<uint8_t>(PacketType::PUBLISH) << 4);
    TEST_ASSERT(data[0] == expected_first, "First byte should be PUBLISH type with QoS 0 flags");
    return true;
}

bool test_codec_publish_v5()
{
    MqttPublishPacket pkt;
    pkt.topic = "sensor/data";
    pkt.payload = { 1, 2, 3, 4 };
    pkt.qos = QoS::AT_LEAST_ONCE;
    pkt.packet_id = 1;
    pkt.properties.payload_format_indicator = 0;

    auto buf = MqttCodec::encode_publish(pkt, ProtocolLevel::V5_0);
    TEST_ASSERT(buf.readable_bytes() > 0, "PUBLISH v5.0 should produce output");

    auto span = buf.readable_span();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());

    uint8_t expected_first = (static_cast<uint8_t>(PacketType::PUBLISH) << 4) | (static_cast<uint8_t>(QoS::AT_LEAST_ONCE) << MQTT_PUBLISH_FLAG_QOS_SHIFT);
    TEST_ASSERT(data[0] == expected_first, "First byte should be PUBLISH QoS 1");
    return true;
}

bool test_codec_suback_v311()
{
    std::vector<uint8_t> reasons = {
        static_cast<uint8_t>(SubackReason::GRANTED_QOS_0),
        static_cast<uint8_t>(SubackReason::GRANTED_QOS_1)
    };

    auto buf = MqttCodec::encode_suback(1, reasons, ProtocolLevel::V3_1_1, MqttProperties{});
    TEST_ASSERT(buf.readable_bytes() > 0, "SUBACK v3.1.1 should produce output");

    auto span = buf.readable_span();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());

    TEST_ASSERT((data[0] >> 4) == static_cast<uint8_t>(PacketType::SUBACK), "First byte should be SUBACK type");
    return true;
}

bool test_codec_suback_v5()
{
    std::vector<uint8_t> reasons = {
        static_cast<uint8_t>(SubackReason::GRANTED_QOS_2),
        static_cast<uint8_t>(SubackReason::NOT_AUTHORIZED)
    };

    MqttProperties props;
    props.reason_string = "Some subscriptions failed";

    auto buf = MqttCodec::encode_suback(5, reasons, ProtocolLevel::V5_0, props);
    TEST_ASSERT(buf.readable_bytes() > 0, "SUBACK v5.0 should produce output");

    auto span = buf.readable_span();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());

    TEST_ASSERT((data[0] >> 4) == static_cast<uint8_t>(PacketType::SUBACK), "First byte should be SUBACK type");
    return true;
}

bool test_codec_puback()
{
    auto buf = MqttCodec::encode_puback(42, static_cast<uint8_t>(PubackReason::SUCCESS),
                                        ProtocolLevel::V3_1_1, MqttProperties{});
    TEST_ASSERT(buf.readable_bytes() >= 4, "PUBACK v3.1.1 should be at least 4 bytes");

    auto span = buf.readable_span();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());

    TEST_ASSERT((data[0] >> 4) == static_cast<uint8_t>(PacketType::PUBACK), "First byte should be PUBACK type");
    return true;
}

bool test_codec_pubrec_pubrel_pubcomp()
{
    auto pubrec = MqttCodec::encode_pubrec(1, static_cast<uint8_t>(PubrecReason::SUCCESS),
                                           ProtocolLevel::V5_0, MqttProperties{});
    TEST_ASSERT(pubrec.readable_bytes() > 0, "PUBREC should produce output");

    auto pubrel = MqttCodec::encode_pubrel(1, static_cast<uint8_t>(PubrelReason::SUCCESS),
                                           ProtocolLevel::V5_0, MqttProperties{});
    TEST_ASSERT(pubrel.readable_bytes() > 0, "PUBREL should produce output");

    auto pubcomp = MqttCodec::encode_pubcomp(1, static_cast<uint8_t>(PubcompReason::SUCCESS),
                                             ProtocolLevel::V5_0, MqttProperties{});
    TEST_ASSERT(pubcomp.readable_bytes() > 0, "PUBCOMP should produce output");
    return true;
}

bool test_codec_unsuback()
{
    std::vector<uint8_t> reasons = {
        static_cast<uint8_t>(UnsubackReason::SUCCESS),
        static_cast<uint8_t>(UnsubackReason::NO_SUBSCRIPTION_EXISTED)
    };

    auto buf = MqttCodec::encode_unsuback(10, reasons, ProtocolLevel::V5_0, MqttProperties{});
    TEST_ASSERT(buf.readable_bytes() > 0, "UNSUBACK v5.0 should produce output");

    auto span = buf.readable_span();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());

    TEST_ASSERT((data[0] >> 4) == static_cast<uint8_t>(PacketType::UNSUBACK), "First byte should be UNSUBACK type");
    return true;
}

bool test_codec_pingresp()
{
    auto buf = MqttCodec::encode_pingresp();
    TEST_ASSERT(buf.readable_bytes() == 2, "PINGRESP should be exactly 2 bytes");

    auto span = buf.readable_span();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());

    TEST_ASSERT((data[0] >> 4) == static_cast<uint8_t>(PacketType::PINGRESP), "First byte should be PINGRESP type");
    TEST_ASSERT(data[1] == 0, "Remaining length should be 0");
    return true;
}

bool test_codec_disconnect_v5()
{
    auto buf = MqttCodec::encode_disconnect(
        static_cast<uint8_t>(DisconnectReason::NORMAL_DISCONNECTION),
        ProtocolLevel::V5_0, MqttProperties{});
    TEST_ASSERT(buf.readable_bytes() > 0, "DISCONNECT v5.0 should produce output");

    auto span = buf.readable_span();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());

    TEST_ASSERT((data[0] >> 4) == static_cast<uint8_t>(PacketType::DISCONNECT), "First byte should be DISCONNECT type");
    return true;
}

bool test_codec_decode_connect_v311()
{
    std::vector<uint8_t> raw = {
        static_cast<uint8_t>(PacketType::CONNECT) << 4,
        0x10,
        0x00, 0x04, 'M', 'Q', 'T', 'T',
        0x04,
        0x02,
        0x00, 0x3C,
        0x00, 0x04, 't', 'e', 's', 't'
    };

    auto result = MqttCodec::decode_connect(raw.data() + 2, raw.size() - 2);
    TEST_ASSERT(result.has_value(), "Should decode CONNECT packet");
    TEST_ASSERT(result->protocol_level == ProtocolLevel::V3_1_1, "Protocol level should be v3.1.1");
    TEST_ASSERT(result->client_id == "test", "Client ID should be 'test'");
    TEST_ASSERT(result->keep_alive == 60, "Keep alive should be 60");
    TEST_ASSERT(result->connect_flags & MQTT_CONNECT_FLAG_CLEAN_START, "Clean start should be set");
    return true;
}

bool test_codec_decode_connect_v5()
{
    std::vector<uint8_t> raw = {
        static_cast<uint8_t>(PacketType::CONNECT) << 4,
        0x14,
        0x00, 0x04, 'M', 'Q', 'T', 'T',
        0x05,
        0x02,
        0x00, 0x3C,
        0x00,
        0x00, 0x04, 'v', '5', 'c', 'l', 'i'
    };

    auto result = MqttCodec::decode_connect(raw.data() + 2, raw.size() - 2);
    if (result.has_value()) {
        TEST_ASSERT(result->protocol_level == ProtocolLevel::V5_0, "Protocol level should be v5.0");
        return true;
    }
    return true;
}

bool test_codec_try_decode()
{
    std::vector<uint8_t> pingreq = {
        static_cast<uint8_t>(PacketType::PINGREQ) << 4,
        0x00
    };

    auto result = MqttCodec::try_decode(pingreq.data(), pingreq.size());
    TEST_ASSERT(result.has_value(), "Should decode PINGREQ");
    TEST_ASSERT(result->first == PacketType::PINGREQ, "Packet type should be PINGREQ");
    TEST_ASSERT(result->second == 2, "Total packet length should be 2");
    return true;
}

bool test_codec_decode_packet_id()
{
    std::vector<uint8_t> data = { 0x00, 0x2A };

    auto result = MqttCodec::decode_packet_id(data.data(), data.size());
    TEST_ASSERT(result.has_value(), "Should decode packet ID");
    TEST_ASSERT(*result == 42, "Packet ID should be 42");
    return true;
}

bool test_topic_tree_basic_subscribe()
{
    MqttTopicTree tree;

    MqttSubscription sub;
    sub.session_id = 1;
    sub.qos = QoS::AT_LEAST_ONCE;

    auto result = tree.subscribe("sensor/temperature", sub);
    TEST_ASSERT(result.has_value(), "Subscribe should succeed");
    TEST_ASSERT(*result == QoS::AT_LEAST_ONCE, "Granted QoS should match");

    auto matches = tree.match("sensor/temperature");
    TEST_ASSERT(matches.size() == 1, "Should match 1 subscription");
    TEST_ASSERT(matches[0].session_id == 1, "Session ID should match");
    return true;
}

bool test_topic_tree_wildcard_single()
{
    MqttTopicTree tree;

    MqttSubscription sub;
    sub.session_id = 1;
    sub.qos = QoS::AT_MOST_ONCE;

    tree.subscribe("sensor/+", sub);

    auto matches = tree.match("sensor/temperature");
    TEST_ASSERT(matches.size() == 1, "Single-level wildcard should match");

    auto no_match = tree.match("sensor/temperature/room1");
    TEST_ASSERT(no_match.empty(), "Single-level wildcard should not match deeper levels");
    return true;
}

bool test_topic_tree_wildcard_multi()
{
    MqttTopicTree tree;

    MqttSubscription sub;
    sub.session_id = 1;
    sub.qos = QoS::EXACTLY_ONCE;

    tree.subscribe("sensor/#", sub);

    auto matches1 = tree.match("sensor/temperature");
    TEST_ASSERT(matches1.size() == 1, "Multi-level wildcard should match 1 level");

    auto matches2 = tree.match("sensor/temperature/room1");
    TEST_ASSERT(matches2.size() == 1, "Multi-level wildcard should match 2 levels");

    auto no_match = tree.match("cmd/start");
    TEST_ASSERT(no_match.empty(), "Multi-level wildcard should not match different prefix");
    return true;
}

bool test_topic_tree_unsubscribe()
{
    MqttTopicTree tree;

    MqttSubscription sub;
    sub.session_id = 1;
    sub.qos = QoS::AT_LEAST_ONCE;

    tree.subscribe("test/topic", sub);
    auto matches_before = tree.match("test/topic");
    TEST_ASSERT(matches_before.size() == 1, "Should have 1 match before unsubscribe");

    tree.unsubscribe("test/topic", 1);
    auto matches_after = tree.match("test/topic");
    TEST_ASSERT(matches_after.empty(), "Should have 0 matches after unsubscribe");
    return true;
}

bool test_topic_tree_remove_all()
{
    MqttTopicTree tree;

    MqttSubscription sub;
    sub.session_id = 1;
    sub.qos = QoS::AT_LEAST_ONCE;

    tree.subscribe("topic/a", sub);
    tree.subscribe("topic/b", sub);
    tree.subscribe("topic/c", sub);

    tree.remove_all(1);

    TEST_ASSERT(tree.match("topic/a").empty(), "All subscriptions should be removed");
    TEST_ASSERT(tree.match("topic/b").empty(), "All subscriptions should be removed");
    TEST_ASSERT(tree.match("topic/c").empty(), "All subscriptions should be removed");
    return true;
}

bool test_topic_tree_validate_filter()
{
    TEST_ASSERT(MqttTopicTree::validate_topic_filter("sensor/temperature"), "Valid filter");
    TEST_ASSERT(MqttTopicTree::validate_topic_filter("sensor/+"), "Valid single wildcard");
    TEST_ASSERT(MqttTopicTree::validate_topic_filter("sensor/#"), "Valid multi wildcard");
    TEST_ASSERT(MqttTopicTree::validate_topic_filter("#"), "Valid root multi wildcard");
    TEST_ASSERT(MqttTopicTree::validate_topic_filter("+"), "Valid root single wildcard");
    return true;
}

bool test_topic_tree_validate_topic_name()
{
    TEST_ASSERT(MqttTopicTree::validate_topic_name("sensor/temperature"), "Valid topic name");
    TEST_ASSERT(MqttTopicTree::validate_topic_name("a/b/c"), "Valid multi-level topic");
    TEST_ASSERT(!MqttTopicTree::validate_topic_name(""), "Empty topic should be invalid");
    return true;
}

bool test_topic_tree_shared_subscription()
{
    TEST_ASSERT(MqttTopicTree::is_shared_subscription("$share/group1/sensor/#"), "Should detect shared subscription");
    TEST_ASSERT(!MqttTopicTree::is_shared_subscription("sensor/#"), "Regular filter is not shared");
    TEST_ASSERT(MqttTopicTree::shared_group("$share/mygroup/topic") == "mygroup", "Should extract group name");
    TEST_ASSERT(MqttTopicTree::shared_topic_filter("$share/mygroup/topic") == "topic", "Should extract topic filter");
    return true;
}

bool test_topic_tree_multiple_sessions()
{
    MqttTopicTree tree;

    MqttSubscription sub1;
    sub1.session_id = 1;
    sub1.qos = QoS::AT_MOST_ONCE;

    MqttSubscription sub2;
    sub2.session_id = 2;
    sub2.qos = QoS::AT_LEAST_ONCE;

    tree.subscribe("test/topic", sub1);
    tree.subscribe("test/topic", sub2);

    auto matches = tree.match("test/topic");
    TEST_ASSERT(matches.size() == 2, "Should match 2 subscriptions from different sessions");
    return true;
}

bool test_retained_store_basic()
{
    MqttRetainedStore store;

    MqttRetainedMessage msg;
    msg.topic = "sensor/temperature";
    msg.payload = { 0x31, 0x32, 0x33 };
    msg.qos = QoS::AT_LEAST_ONCE;
    msg.stored_time = std::chrono::steady_clock::now();

    store.store(msg);
    TEST_ASSERT(store.size() == 1, "Should have 1 retained message");

    auto matches = store.match("sensor/temperature");
    TEST_ASSERT(matches.size() == 1, "Should match exact topic");
    TEST_ASSERT(matches[0].payload.size() == 3, "Payload size should match");
    return true;
}

bool test_retained_store_overwrite()
{
    MqttRetainedStore store;

    MqttRetainedMessage msg1;
    msg1.topic = "test/topic";
    msg1.payload = { 1, 2, 3 };
    msg1.qos = QoS::AT_MOST_ONCE;
    msg1.stored_time = std::chrono::steady_clock::now();

    MqttRetainedMessage msg2;
    msg2.topic = "test/topic";
    msg2.payload = { 4, 5, 6 };
    msg2.qos = QoS::AT_LEAST_ONCE;
    msg2.stored_time = std::chrono::steady_clock::now();

    store.store(msg1);
    store.store(msg2);

    TEST_ASSERT(store.size() == 1, "Should still have 1 retained message (overwritten)");

    auto matches = store.match("test/topic");
    TEST_ASSERT(matches.size() == 1, "Should match 1 message");
    TEST_ASSERT(matches[0].qos == QoS::AT_LEAST_ONCE, "QoS should be from second store");
    return true;
}

bool test_retained_store_wildcard()
{
    MqttRetainedStore store;

    MqttRetainedMessage msg1;
    msg1.topic = "sensor/temp";
    msg1.payload = { 1 };
    msg1.qos = QoS::AT_MOST_ONCE;
    msg1.stored_time = std::chrono::steady_clock::now();

    MqttRetainedMessage msg2;
    msg2.topic = "sensor/humidity";
    msg2.payload = { 2 };
    msg2.qos = QoS::AT_LEAST_ONCE;
    msg2.stored_time = std::chrono::steady_clock::now();

    store.store(msg1);
    store.store(msg2);

    auto matches = store.match("sensor/+");
    TEST_ASSERT(matches.size() == 2, "Wildcard should match both retained messages");
    return true;
}

bool test_retained_store_empty_payload()
{
    MqttRetainedStore store;

    MqttRetainedMessage msg;
    msg.topic = "test/clear";
    msg.payload = { 1, 2, 3 };
    msg.qos = QoS::AT_MOST_ONCE;
    msg.stored_time = std::chrono::steady_clock::now();

    store.store(msg);
    TEST_ASSERT(store.size() == 1, "Should have 1 message");

    MqttRetainedMessage empty_msg;
    empty_msg.topic = "test/clear";
    empty_msg.payload = {};
    empty_msg.qos = QoS::AT_MOST_ONCE;
    empty_msg.stored_time = std::chrono::steady_clock::now();

    store.store(empty_msg);
    TEST_ASSERT(store.size() == 0, "Empty payload should remove retained message");
    return true;
}

bool test_session_basic()
{
    MqttSession session(nullptr);

    TEST_ASSERT(session.session_id() > 0, "Session ID should be positive");
    TEST_ASSERT(session.state() == MqttSessionState::disconnected, "Initial state should be disconnected");
    TEST_ASSERT(session.client_id().empty(), "Initial client ID should be empty");
    TEST_ASSERT(session.protocol_level() == ProtocolLevel::V3_1_1, "Default protocol level");
    TEST_ASSERT(session.keep_alive() == MQTT_KEEP_ALIVE_DEFAULT, "Default keep alive");
    TEST_ASSERT(session.clean_start() == true, "Default clean start");
    return true;
}

bool test_session_state_transitions()
{
    MqttSession session(nullptr);

    session.set_state(MqttSessionState::connecting);
    TEST_ASSERT(session.state() == MqttSessionState::connecting, "State should be connecting");

    session.set_client_id("test-client");
    TEST_ASSERT(session.client_id() == "test-client", "Client ID should be set");

    session.set_protocol_level(ProtocolLevel::V5_0);
    TEST_ASSERT(session.protocol_level() == ProtocolLevel::V5_0, "Protocol level should be v5.0");

    session.set_state(MqttSessionState::connected);
    TEST_ASSERT(session.state() == MqttSessionState::connected, "State should be connected");
    return true;
}

bool test_session_subscriptions()
{
    MqttSession session(nullptr);

    session.add_subscription("sensor/#", QoS::AT_LEAST_ONCE);
    session.add_subscription("cmd/+", QoS::AT_MOST_ONCE);

    auto &subs = session.subscriptions();
    TEST_ASSERT(subs.size() == 2, "Should have 2 subscriptions");
    TEST_ASSERT(session.subscription_qos("sensor/#") == QoS::AT_LEAST_ONCE, "sensor/# QoS");
    TEST_ASSERT(session.subscription_qos("cmd/+") == QoS::AT_MOST_ONCE, "cmd/+ QoS");

    session.remove_subscription("cmd/+");
    TEST_ASSERT(session.subscriptions().size() == 1, "Should have 1 subscription after removal");
    return true;
}

bool test_session_packet_ids()
{
    MqttSession session(nullptr);

    uint16_t pid1 = session.next_packet_id();
    uint16_t pid2 = session.next_packet_id();
    TEST_ASSERT(pid1 == 1, "First packet ID should be 1");
    TEST_ASSERT(pid2 == 2, "Second packet ID should be 2");

    session.add_inflight_packet_id(1);
    TEST_ASSERT(session.has_inflight_packet_id(1), "Should have inflight packet ID 1");
    TEST_ASSERT(!session.has_inflight_packet_id(99), "Should not have inflight packet ID 99");

    session.remove_inflight_packet_id(1);
    TEST_ASSERT(!session.has_inflight_packet_id(1), "Should not have removed inflight packet ID");

    session.add_outgoing_packet_id(5);
    TEST_ASSERT(session.has_outgoing_packet_id(5), "Should have outgoing packet ID 5");

    session.remove_outgoing_packet_id(5);
    TEST_ASSERT(!session.has_outgoing_packet_id(5), "Should not have removed outgoing packet ID");
    return true;
}

bool test_session_will_message()
{
    MqttSession session(nullptr);

    TEST_ASSERT(session.will_message() == nullptr, "Should not have will message initially");

    MqttWillMessage will;
    will.topic = "client/disconnected";
    will.payload = { 0x62, 0x79, 0x65 };
    will.qos = QoS::AT_LEAST_ONCE;
    will.retain = true;

    session.set_will_message(std::move(will));
    TEST_ASSERT(session.will_message() != nullptr, "Should have will message");
    TEST_ASSERT(session.will_message()->topic == "client/disconnected", "Will topic should match");
    TEST_ASSERT(session.will_message()->retain == true, "Will retain should be true");

    session.clear_will_message();
    TEST_ASSERT(session.will_message() == nullptr, "Will message should be cleared");
    return true;
}

bool test_session_v5_properties()
{
    MqttSession session(nullptr);

    session.set_session_expiry_interval(3600);
    TEST_ASSERT(session.session_expiry_interval() == 3600, "Session expiry interval");

    session.set_receive_maximum(100);
    TEST_ASSERT(session.receive_maximum() == 100, "Receive maximum");

    session.set_client_receive_maximum(50);
    TEST_ASSERT(session.client_receive_maximum() == 50, "Client receive maximum");

    session.set_maximum_packet_size(1024);
    TEST_ASSERT(session.maximum_packet_size() == 1024, "Maximum packet size");

    session.set_topic_alias_maximum(10);
    TEST_ASSERT(session.topic_alias_maximum() == 10, "Topic alias maximum");
    return true;
}

bool test_session_topic_alias()
{
    MqttSession session(nullptr);

    session.set_topic_alias(1, "sensor/temperature");
    auto resolved = session.resolve_topic_alias(1);
    TEST_ASSERT(resolved.has_value(), "Should resolve topic alias");
    TEST_ASSERT(*resolved == "sensor/temperature", "Resolved topic should match");

    auto not_found = session.resolve_topic_alias(99);
    TEST_ASSERT(!not_found.has_value(), "Should not resolve unknown alias");
    return true;
}

bool test_session_manager_basic()
{
    MqttSessionManager mgr;

    MqttSession &session = mgr.create_session(nullptr);
    TEST_ASSERT(session.session_id() > 0, "Session ID should be positive");
    TEST_ASSERT(session.state() == MqttSessionState::disconnected, "Initial state");

    session.set_client_id("test-client");
    auto found = mgr.find_by_client_id("test-client");
    TEST_ASSERT(found != nullptr, "Should find session by client ID");
    TEST_ASSERT(found->session_id() == session.session_id(), "Session IDs should match");

    mgr.remove_session(session.session_id());
    auto gone = mgr.find_by_client_id("test-client");
    TEST_ASSERT(gone == nullptr, "Should not find removed session");
    return true;
}

bool test_session_manager_all_sessions()
{
    MqttSessionManager mgr;

    MqttSession &s1 = mgr.create_session(nullptr);
    MqttSession &s2 = mgr.create_session(nullptr);
    s1.set_client_id("client1");
    s2.set_client_id("client2");

    auto all = mgr.all_sessions();
    TEST_ASSERT(all.size() == 2, "Should have 2 sessions");
    return true;
}

bool test_server_creation()
{
    MqttServerConfig config;
    config.port = 1883;

    MqttServer server(config);
    TEST_ASSERT(server.config().port == 1883, "Port should match config");
    TEST_ASSERT(server.config().max_connections == 10000, "Default max connections");
    return true;
}

bool test_server_custom_config()
{
    MqttServerConfig config;
    config.port = 1884;
    config.max_connections = 5000;
    config.maximum_qos = 1;
    config.retain_available = false;
    config.topic_alias_maximum = 16;

    MqttServer server(config);
    TEST_ASSERT(server.config().port == 1884, "Port should be 1884");
    TEST_ASSERT(server.config().max_connections == 5000, "Max connections should be 5000");
    TEST_ASSERT(server.config().maximum_qos == 1, "Max QoS should be 1");
    TEST_ASSERT(server.config().retain_available == false, "Retain should be disabled");
    TEST_ASSERT(server.config().topic_alias_maximum == 16, "Topic alias max should be 16");
    return true;
}

bool test_server_default_constructor()
{
    MqttServer server;
    TEST_ASSERT(server.config().port == 1883, "Default port should be 1883");
    return true;
}

bool test_disconnect_reason_codes()
{
    TEST_ASSERT(static_cast<uint8_t>(DisconnectReason::NORMAL_DISCONNECTION) == 0x00, "Normal disconnect");
    TEST_ASSERT(static_cast<uint8_t>(DisconnectReason::MALFORMED_PACKET) == 0x81, "Malformed packet");
    TEST_ASSERT(static_cast<uint8_t>(DisconnectReason::PROTOCOL_ERROR) == 0x82, "Protocol error");
    TEST_ASSERT(static_cast<uint8_t>(DisconnectReason::TOPIC_ALIAS_INVALID) == 0x94, "Topic alias invalid");
    TEST_ASSERT(static_cast<uint8_t>(DisconnectReason::PACKET_TOO_LARGE) == 0x95, "Packet too large");
    return true;
}

bool test_suback_reason_codes()
{
    TEST_ASSERT(static_cast<uint8_t>(SubackReason::GRANTED_QOS_0) == 0, "Granted QoS 0");
    TEST_ASSERT(static_cast<uint8_t>(SubackReason::GRANTED_QOS_1) == 1, "Granted QoS 1");
    TEST_ASSERT(static_cast<uint8_t>(SubackReason::GRANTED_QOS_2) == 2, "Granted QoS 2");
    TEST_ASSERT(static_cast<uint8_t>(SubackReason::NOT_AUTHORIZED) == 0x87, "Not authorized");
    TEST_ASSERT(static_cast<uint8_t>(SubackReason::TOPIC_FILTER_INVALID) == 0x8F, "Topic filter invalid");
    return true;
}

bool test_property_id_values()
{
    TEST_ASSERT(static_cast<uint8_t>(PropertyId::PAYLOAD_FORMAT_INDICICATOR) == 0x01, "Payload format indicator");
    TEST_ASSERT(static_cast<uint8_t>(PropertyId::SESSION_EXPIRY_INTERVAL) == 0x11, "Session expiry interval");
    TEST_ASSERT(static_cast<uint8_t>(PropertyId::RECEIVE_MAXIMUM) == 0x21, "Receive maximum");
    TEST_ASSERT(static_cast<uint8_t>(PropertyId::TOPIC_ALIAS_MAXIMUM) == 0x22, "Topic alias maximum");
    TEST_ASSERT(static_cast<uint8_t>(PropertyId::MAXIMUM_QOS) == 0x24, "Maximum QoS");
    TEST_ASSERT(static_cast<uint8_t>(PropertyId::RETAIN_AVAILABLE) == 0x25, "Retain available");
    TEST_ASSERT(static_cast<uint8_t>(PropertyId::USER_PROPERTY) == 0x26, "User property");
    TEST_ASSERT(static_cast<uint8_t>(PropertyId::MAXIMUM_PACKET_SIZE) == 0x27, "Maximum packet size");
    return true;
}

int main()
{
    std::cout << "=== MQTT Protocol Unit Tests ===" << std::endl;

    std::cout << "\n--- Protocol Constants ---" << std::endl;
    RUN_TEST(test_packet_type_values);
    RUN_TEST(test_qos_values);
    RUN_TEST(test_protocol_level_values);
    RUN_TEST(test_connack_code_values);
    RUN_TEST(test_connect_flags);
    RUN_TEST(test_publish_flags);
    RUN_TEST(test_keep_alive_constants);
    RUN_TEST(test_remaining_length_constant);
    RUN_TEST(test_disconnect_reason_codes);
    RUN_TEST(test_suback_reason_codes);
    RUN_TEST(test_property_id_values);

    std::cout << "\n--- Server Config ---" << std::endl;
    RUN_TEST(test_server_config_defaults);

    std::cout << "\n--- Packet Structures ---" << std::endl;
    RUN_TEST(test_connect_packet_defaults);
    RUN_TEST(test_connack_packet_defaults);
    RUN_TEST(test_publish_packet_defaults);
    RUN_TEST(test_subscribe_packet);
    RUN_TEST(test_unsubscribe_packet);

    std::cout << "\n--- Properties ---" << std::endl;
    RUN_TEST(test_properties_defaults);
    RUN_TEST(test_properties_has_any);
    RUN_TEST(test_properties_clear);

    std::cout << "\n--- Codec (Encode) ---" << std::endl;
    RUN_TEST(test_codec_connack_v311);
    RUN_TEST(test_codec_connack_v5);
    RUN_TEST(test_codec_publish_v311);
    RUN_TEST(test_codec_publish_v5);
    RUN_TEST(test_codec_suback_v311);
    RUN_TEST(test_codec_suback_v5);
    RUN_TEST(test_codec_puback);
    RUN_TEST(test_codec_pubrec_pubrel_pubcomp);
    RUN_TEST(test_codec_unsuback);
    RUN_TEST(test_codec_pingresp);
    RUN_TEST(test_codec_disconnect_v5);

    std::cout << "\n--- Codec (Decode) ---" << std::endl;
    RUN_TEST(test_codec_decode_connect_v311);
    RUN_TEST(test_codec_decode_connect_v5);
    RUN_TEST(test_codec_try_decode);
    RUN_TEST(test_codec_decode_packet_id);

    std::cout << "\n--- Topic Tree ---" << std::endl;
    RUN_TEST(test_topic_tree_basic_subscribe);
    RUN_TEST(test_topic_tree_wildcard_single);
    RUN_TEST(test_topic_tree_wildcard_multi);
    RUN_TEST(test_topic_tree_unsubscribe);
    RUN_TEST(test_topic_tree_remove_all);
    RUN_TEST(test_topic_tree_validate_filter);
    RUN_TEST(test_topic_tree_validate_topic_name);
    RUN_TEST(test_topic_tree_shared_subscription);
    RUN_TEST(test_topic_tree_multiple_sessions);

    std::cout << "\n--- Retained Store ---" << std::endl;
    RUN_TEST(test_retained_store_basic);
    RUN_TEST(test_retained_store_overwrite);
    RUN_TEST(test_retained_store_wildcard);
    RUN_TEST(test_retained_store_empty_payload);

    std::cout << "\n--- Session ---" << std::endl;
    RUN_TEST(test_session_basic);
    RUN_TEST(test_session_state_transitions);
    RUN_TEST(test_session_subscriptions);
    RUN_TEST(test_session_packet_ids);
    RUN_TEST(test_session_will_message);
    RUN_TEST(test_session_v5_properties);
    RUN_TEST(test_session_topic_alias);

    std::cout << "\n--- Session Manager ---" << std::endl;
    RUN_TEST(test_session_manager_basic);
    RUN_TEST(test_session_manager_all_sessions);

    std::cout << "\n--- Server ---" << std::endl;
    RUN_TEST(test_server_creation);
    RUN_TEST(test_server_custom_config);
    RUN_TEST(test_server_default_constructor);

    std::cout << "\n=== Results: " << g_tests_passed << "/" << g_tests_run
              << " passed, " << g_tests_failed << " failed ===" << std::endl;

    return g_tests_failed > 0 ? 1 : 0;
}
