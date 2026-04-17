#ifndef __NET_MQTT_MQTT_CONFIG_H__
#define __NET_MQTT_MQTT_CONFIG_H__

#include "mqtt_protocol.h"
#include <cstdint>
#include <vector>

namespace yuan::net::mqtt
{
    struct MqttServerConfig
    {
        uint16_t port = 1883;
        uint32_t max_connections = 10000;
        uint32_t max_message_size = 256 * 1024;
        uint32_t max_packet_size = 256 * 1024;
        uint16_t keep_alive_default = MQTT_KEEP_ALIVE_DEFAULT;
        double keep_alive_factor = MQTT_KEEP_ALIVE_FACTOR;
        uint16_t topic_alias_maximum = 0;
        uint16_t receive_maximum = 65535;
        uint8_t maximum_qos = 2;
        bool retain_available = true;
        bool wildcard_subscription_available = true;
        bool subscription_identifier_available = true;
        bool shared_subscription_available = true;
        bool require_authentication = false;
        uint32_t idle_timeout_ms = 0;
        std::vector<ProtocolLevel> supported_versions = {
            ProtocolLevel::V3_1_1, ProtocolLevel::V5_0
        };
    };
}

#endif
