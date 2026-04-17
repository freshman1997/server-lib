#ifndef __NET_MQTT_MQTT_PROTOCOL_H__
#define __NET_MQTT_MQTT_PROTOCOL_H__

#include <cstddef>
#include <cstdint>

namespace yuan::net::mqtt
{
    enum class PacketType : uint8_t {
        CONNECT = 1,
        CONNACK = 2,
        PUBLISH = 3,
        PUBACK = 4,
        PUBREC = 5,
        PUBREL = 6,
        PUBCOMP = 7,
        SUBSCRIBE = 8,
        SUBACK = 9,
        UNSUBSCRIBE = 10,
        UNSUBACK = 11,
        PINGREQ = 12,
        PINGRESP = 13,
        DISCONNECT = 14,
        AUTH = 15
    };

    enum class QoS : uint8_t {
        AT_MOST_ONCE = 0,
        AT_LEAST_ONCE = 1,
        EXACTLY_ONCE = 2
    };

    enum class ProtocolLevel : uint8_t {
        V3_1_1 = 4,
        V5_0 = 5
    };

    enum class ConnackCode : uint8_t {
        ACCEPTED = 0,
        UNACCEPTABLE_PROTOCOL_VERSION = 1,
        IDENTIFIER_REJECTED = 2,
        SERVER_UNAVAILABLE = 3,
        BAD_USER_NAME_OR_PASSWORD = 4,
        NOT_AUTHORIZED = 5,
        V5_UNSPECIFIED_ERROR = 0x80,
        V5_MALFORMED_PACKET = 0x81,
        V5_PROTOCOL_ERROR = 0x82,
        V5_IMPLEMENTATION_SPECIFIC_ERROR = 0x83,
        V5_UNSUPPORTED_PROTOCOL_VERSION = 0x84,
        V5_CLIENT_IDENTIFIER_NOT_VALID = 0x85,
        V5_BAD_USER_NAME_OR_PASSWORD = 0x86,
        V5_NOT_AUTHORIZED = 0x87,
        V5_SERVER_UNAVAILABLE = 0x88,
        V5_SERVER_BUSY = 0x89,
        V5_BANNED = 0x8A,
        V5_BAD_AUTHENTICATION_METHOD = 0x8C,
        V5_TOPIC_NAME_INVALID = 0x90,
        V5_PACKET_TOO_LARGE = 0x95,
        V5_QUOTA_EXCEEDED = 0x97,
        V5_RETAIN_NOT_SUPPORTED = 0x9A,
        V5_CONNECTION_RATE_EXCEEDED = 0x9F
    };

    enum class SubackReason : uint8_t {
        GRANTED_QOS_0 = 0,
        GRANTED_QOS_1 = 1,
        GRANTED_QOS_2 = 2,
        UNSPECIFIED_ERROR = 0x80,
        IMPLEMENTATION_SPECIFIC_ERROR = 0x83,
        NOT_AUTHORIZED = 0x87,
        TOPIC_FILTER_INVALID = 0x8F,
        PACKET_IDENTIFIER_IN_USE = 0x91,
        QUOTA_EXCEEDED = 0x97,
        SHARED_SUBSCRIPTION_NOT_SUPPORTED = 0x9E,
        SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED = 0xA1,
        WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED = 0xA2
    };

    enum class UnsubackReason : uint8_t {
        SUCCESS = 0,
        NO_SUBSCRIPTION_EXISTED = 0x11,
        UNSPECIFIED_ERROR = 0x80,
        IMPLEMENTATION_SPECIFIC_ERROR = 0x83,
        NOT_AUTHORIZED = 0x87,
        TOPIC_FILTER_INVALID = 0x8F,
        PACKET_IDENTIFIER_IN_USE = 0x91
    };

    enum class PubackReason : uint8_t {
        SUCCESS = 0,
        NO_MATCHING_SUBSCRIBERS = 0x10,
        UNSPECIFIED_ERROR = 0x80,
        IMPLEMENTATION_SPECIFIC_ERROR = 0x83,
        NOT_AUTHORIZED = 0x87,
        TOPIC_NAME_INVALID = 0x90,
        PACKET_IDENTIFIER_IN_USE = 0x91,
        QUOTA_EXCEEDED = 0x97,
        PAYLOAD_FORMAT_INVALID = 0x99
    };

    enum class PubrecReason : uint8_t {
        SUCCESS = 0,
        NO_MATCHING_SUBSCRIBERS = 0x10,
        UNSPECIFIED_ERROR = 0x80,
        IMPLEMENTATION_SPECIFIC_ERROR = 0x83,
        NOT_AUTHORIZED = 0x87,
        TOPIC_NAME_INVALID = 0x90,
        PACKET_IDENTIFIER_IN_USE = 0x91,
        QUOTA_EXCEEDED = 0x97,
        PAYLOAD_FORMAT_INVALID = 0x99
    };

    enum class PubrelReason : uint8_t {
        SUCCESS = 0,
        PACKET_IDENTIFIER_NOT_FOUND = 0x92
    };

    enum class PubcompReason : uint8_t {
        SUCCESS = 0,
        PACKET_IDENTIFIER_NOT_FOUND = 0x92
    };

    enum class DisconnectReason : uint8_t {
        NORMAL_DISCONNECTION = 0x00,
        DISCONNECT_WITH_WILL_MESSAGE = 0x04,
        UNSPECIFIED_ERROR = 0x80,
        MALFORMED_PACKET = 0x81,
        PROTOCOL_ERROR = 0x82,
        IMPLEMENTATION_SPECIFIC_ERROR = 0x83,
        RECEIVE_MAXIMUM_EXCEEDED = 0x93,
        TOPIC_ALIAS_INVALID = 0x94,
        PACKET_TOO_LARGE = 0x95,
        MESSAGE_RATE_TOO_HIGH = 0x96,
        QUOTA_EXCEEDED = 0x97,
        ADMINISTRATIVE_ACTION = 0x98,
        PAYLOAD_FORMAT_INVALID = 0x99,
        RETAIN_NOT_SUPPORTED = 0x9A,
        QOS_NOT_SUPPORTED = 0x9B,
        USE_ANOTHER_SERVER = 0x9C,
        SERVER_MOVED = 0x9D,
        SHARED_SUBSCRIPTION_NOT_SUPPORTED = 0x9E,
        CONNECTION_RATE_EXCEEDED = 0x9F,
        MAX_CONNECT_TIME = 0xA0,
        SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED = 0xA1,
        WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED = 0xA2
    };

    enum class AuthReason : uint8_t {
        SUCCESS = 0,
        CONTINUE_AUTHENTICATION = 0x18,
        RE_AUTHENTICATE = 0x19
    };

    enum class PropertyId : uint8_t {
        PAYLOAD_FORMAT_INDICICATOR = 0x01,
        MESSAGE_EXPIRY_INTERVAL = 0x02,
        CONTENT_TYPE = 0x03,
        RESPONSE_TOPIC = 0x08,
        CORRELATION_DATA = 0x09,
        SUBSCRIPTION_IDENTIFIER = 0x0B,
        SESSION_EXPIRY_INTERVAL = 0x11,
        ASSIGNED_CLIENT_IDENTIFIER = 0x12,
        SERVER_KEEP_ALIVE = 0x13,
        AUTHENTICATION_METHOD = 0x15,
        AUTHENTICATION_DATA = 0x16,
        REQUEST_PROBLEM_INFORMATION = 0x17,
        WILL_DELAY_INTERVAL = 0x18,
        REQUEST_RESPONSE_INFORMATION = 0x19,
        RESPONSE_INFORMATION = 0x1A,
        SERVER_REFERENCE = 0x1C,
        REASON_STRING = 0x1F,
        RECEIVE_MAXIMUM = 0x21,
        TOPIC_ALIAS_MAXIMUM = 0x22,
        TOPIC_ALIAS = 0x23,
        MAXIMUM_QOS = 0x24,
        RETAIN_AVAILABLE = 0x25,
        USER_PROPERTY = 0x26,
        MAXIMUM_PACKET_SIZE = 0x27,
        WILDCARD_SUBSCRIPTION_AVAILABLE = 0x28,
        SUBSCRIPTION_IDENTIFIER_AVAILABLE = 0x29,
        SHARED_SUBSCRIPTION_AVAILABLE = 0x2A
    };

    constexpr uint8_t MQTT_CONNECT_FLAG_USERNAME = 0x80;
    constexpr uint8_t MQTT_CONNECT_FLAG_PASSWORD = 0x40;
    constexpr uint8_t MQTT_CONNECT_FLAG_WILL_RETAIN = 0x20;
    constexpr uint8_t MQTT_CONNECT_FLAG_WILL_QOS_SHIFT = 3;
    constexpr uint8_t MQTT_CONNECT_FLAG_WILL_FLAG = 0x04;
    constexpr uint8_t MQTT_CONNECT_FLAG_CLEAN_START = 0x02;

    constexpr uint8_t MQTT_PUBLISH_FLAG_DUP = 0x08;
    constexpr uint8_t MQTT_PUBLISH_FLAG_QOS_SHIFT = 1;
    constexpr uint8_t MQTT_PUBLISH_FLAG_RETAIN = 0x01;

    constexpr uint16_t MQTT_KEEP_ALIVE_DEFAULT = 60;
    constexpr double MQTT_KEEP_ALIVE_FACTOR = 1.5;
    constexpr uint32_t MQTT_MAX_REMAINING_LENGTH = 268435455;
    constexpr uint16_t MQTT_PACKET_ID_MAX = 65535;
    constexpr size_t MQTT_VARIABLE_BYTE_INT_MAX_SIZE = 4;

    constexpr const char *MQTT_PROTOCOL_NAME_V3 = "MQTT";
    constexpr const char *MQTT_PROTOCOL_NAME_V5 = "MQTT";
    constexpr uint16_t MQTT_PROTOCOL_NAME_LENGTH = 4;
}

#endif
