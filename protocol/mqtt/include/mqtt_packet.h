#ifndef __NET_MQTT_MQTT_PACKET_H__
#define __NET_MQTT_MQTT_PACKET_H__

#include "mqtt_protocol.h"
#include "mqtt_properties.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::mqtt
{
    struct MqttConnectPacket
    {
        ProtocolLevel protocol_level = ProtocolLevel::V3_1_1;
        uint8_t connect_flags = 0;
        uint16_t keep_alive = MQTT_KEEP_ALIVE_DEFAULT;

        std::string client_id;
        std::optional<std::string> will_topic;
        std::optional<std::vector<uint8_t> > will_payload;
        std::optional<std::string> username;
        std::optional<std::string> password;

        MqttProperties properties;
        MqttProperties will_properties;
    };

    struct MqttConnackPacket
    {
        uint8_t session_present = 0;
        uint8_t reason_code = 0;
        MqttProperties properties;
    };

    struct MqttPublishPacket
    {
        uint8_t dup = 0;
        QoS qos = QoS::AT_MOST_ONCE;
        uint8_t retain = 0;
        std::string topic;
        std::optional<uint16_t> packet_id;
        std::vector<uint8_t> payload;
        MqttProperties properties;
    };

    struct MqttPubackPacket
    {
        uint16_t packet_id = 0;
        uint8_t reason_code = 0;
        MqttProperties properties;
    };

    struct MqttPubrecPacket
    {
        uint16_t packet_id = 0;
        uint8_t reason_code = 0;
        MqttProperties properties;
    };

    struct MqttPubrelPacket
    {
        uint16_t packet_id = 0;
        uint8_t reason_code = 0;
        MqttProperties properties;
    };

    struct MqttPubcompPacket
    {
        uint16_t packet_id = 0;
        uint8_t reason_code = 0;
        MqttProperties properties;
    };

    struct MqttSubOption
    {
        std::string topic_filter;
        QoS maximum_qos = QoS::AT_MOST_ONCE;
        uint8_t no_local = 0;
        uint8_t retain_as_published = 0;
        uint8_t retain_handling = 0;
    };

    struct MqttSubscribePacket
    {
        uint16_t packet_id = 0;
        std::vector<MqttSubOption> subscriptions;
        MqttProperties properties;
    };

    struct MqttUnsubscribePacket
    {
        uint16_t packet_id = 0;
        std::vector<std::string> topic_filters;
        MqttProperties properties;
    };

    struct MqttUnsubackPacket
    {
        uint16_t packet_id = 0;
        std::vector<uint8_t> reason_codes;
        MqttProperties properties;
    };

    struct MqttDisconnectPacket
    {
        uint8_t reason_code = 0;
        MqttProperties properties;
    };

    struct MqttAuthPacket
    {
        uint8_t reason_code = 0;
        MqttProperties properties;
    };
}

#endif
