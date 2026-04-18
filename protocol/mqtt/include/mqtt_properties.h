#ifndef __NET_MQTT_MQTT_PROPERTIES_H__
#define __NET_MQTT_MQTT_PROPERTIES_H__

#include "mqtt_protocol.h"
#include "buffer/byte_buffer.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::mqtt
{
    using ByteBuffer = ::yuan::buffer::ByteBuffer;

    struct UserProperty
    {
        std::string key;
        std::string value;
    };

    struct MqttProperties
    {
        std::optional<uint8_t> payload_format_indicator;
        std::optional<uint32_t> message_expiry_interval;
        std::optional<std::string> content_type;
        std::optional<std::string> response_topic;
        std::optional<std::vector<uint8_t> > correlation_data;
        std::optional<uint32_t> subscription_identifier;
        std::optional<uint32_t> session_expiry_interval;
        std::optional<std::string> assigned_client_identifier;
        std::optional<uint16_t> server_keep_alive;
        std::optional<std::string> authentication_method;
        std::optional<std::vector<uint8_t> > authentication_data;
        std::optional<uint8_t> request_problem_information;
        std::optional<uint32_t> will_delay_interval;
        std::optional<uint8_t> request_response_information;
        std::optional<std::string> response_information;
        std::optional<std::string> server_reference;
        std::optional<std::string> reason_string;
        std::optional<uint16_t> receive_maximum;
        std::optional<uint16_t> topic_alias_maximum;
        std::optional<uint16_t> topic_alias;
        std::optional<uint8_t> maximum_qos;
        std::optional<uint8_t> retain_available;
        std::optional<uint32_t> maximum_packet_size;
        std::optional<uint8_t> wildcard_subscription_available;
        std::optional<uint8_t> subscription_identifier_available;
        std::optional<uint8_t> shared_subscription_available;
        std::vector<UserProperty> user_properties;

        size_t encoded_size() const;
        bool has_any() const;
        void clear();
    };

    size_t decode_properties(const uint8_t * data, size_t len, size_t & offset, MqttProperties & props);
    void encode_properties(ByteBuffer & buf, const MqttProperties & props);
}

#endif
