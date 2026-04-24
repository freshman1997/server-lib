#ifndef __NET_MQTT_MQTT_CODEC_H__
#define __NET_MQTT_MQTT_CODEC_H__

#include "mqtt_protocol.h"
#include "mqtt_packet.h"
#include "mqtt_properties.h"
#include "buffer/byte_buffer.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace yuan::net::mqtt
{
    using ByteBuffer = ::yuan::buffer::ByteBuffer;

    class MqttCodec
    {
    public:
        static std::optional<std::pair<PacketType, size_t> >
        try_decode(const uint8_t *data, size_t len);

        static std::optional<MqttConnectPacket> decode_connect(const uint8_t *data, size_t len);
        static std::optional<MqttPublishPacket> decode_publish(const uint8_t *data, size_t len, uint8_t flags, ProtocolLevel level);
        static std::optional<MqttSubscribePacket> decode_subscribe(const uint8_t *data, size_t len, ProtocolLevel level);
        static std::optional<MqttUnsubscribePacket> decode_unsubscribe(const uint8_t *data, size_t len, ProtocolLevel level);
        static std::optional<uint16_t> decode_packet_id(const uint8_t *data, size_t len);
        static std::optional<MqttPubackPacket> decode_puback(const uint8_t *data, size_t len, ProtocolLevel level);
        static std::optional<MqttPubrecPacket> decode_pubrec(const uint8_t *data, size_t len, ProtocolLevel level);
        static std::optional<MqttPubrelPacket> decode_pubrel(const uint8_t *data, size_t len, ProtocolLevel level);
        static std::optional<MqttPubcompPacket> decode_pubcomp(const uint8_t *data, size_t len, ProtocolLevel level);
        static std::optional<MqttDisconnectPacket> decode_disconnect(const uint8_t *data, size_t len);
        static std::optional<MqttAuthPacket> decode_auth(const uint8_t *data, size_t len);

        static ByteBuffer encode_connack(const MqttConnackPacket &pkt, ProtocolLevel level);
        static ByteBuffer encode_publish(const MqttPublishPacket &pkt, ProtocolLevel level);
        static ByteBuffer encode_suback(uint16_t packet_id, const std::vector<uint8_t> &reason_codes, ProtocolLevel level, const MqttProperties &props);
        static ByteBuffer encode_puback(uint16_t packet_id, uint8_t reason_code, ProtocolLevel level, const MqttProperties &props);
        static ByteBuffer encode_pubrec(uint16_t packet_id, uint8_t reason_code, ProtocolLevel level, const MqttProperties &props);
        static ByteBuffer encode_pubrel(uint16_t packet_id, uint8_t reason_code, ProtocolLevel level, const MqttProperties &props);
        static ByteBuffer encode_pubcomp(uint16_t packet_id, uint8_t reason_code, ProtocolLevel level, const MqttProperties &props);
        static ByteBuffer encode_unsuback(uint16_t packet_id, const std::vector<uint8_t> &reason_codes, ProtocolLevel level, const MqttProperties &props);
        static ByteBuffer encode_pingresp();
        static ByteBuffer encode_disconnect(uint8_t reason_code, ProtocolLevel level, const MqttProperties &props);

        static ByteBuffer build_fixed_header(PacketType type, uint8_t flags, size_t remaining_length);

    private:
        static size_t encode_remaining_length(uint32_t value, uint8_t *out);
        static std::optional<uint32_t> decode_remaining_length(const uint8_t *data, size_t len, size_t &bytes_consumed);

        static std::optional<std::string> read_utf8_string(const uint8_t *data, size_t len, size_t &offset);
        static void write_utf8_string(ByteBuffer &buf, const std::string &str);

        static std::optional<std::vector<uint8_t> > read_binary_data(const uint8_t *data, size_t len, size_t &offset);
        static void write_binary_data(ByteBuffer &buf, const std::vector<uint8_t> &data);
    };
}

#endif
