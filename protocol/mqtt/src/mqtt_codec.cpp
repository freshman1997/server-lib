#include "mqtt_codec.h"
#include <cstring>

namespace yuan::net::mqtt
{

    size_t MqttCodec::encode_remaining_length(uint32_t value, uint8_t * out)
    {
        size_t len = 0;
        do {
            uint8_t byte = value & 0x7F;
            value >>= 7;
            if (value > 0)
                byte |= 0x80;
            out[len++] = byte;
        } while (value > 0);
        return len;
    }

    std::optional<uint32_t> MqttCodec::decode_remaining_length(const uint8_t * data, size_t len, size_t & bytes_consumed)
    {
        uint32_t value = 0;
        uint32_t multiplier = 1;
        bytes_consumed = 0;
        for (size_t i = 0; i < MQTT_VARIABLE_BYTE_INT_MAX_SIZE && i < len; ++i) {
            uint8_t byte = data[i];
            value += (static_cast<uint32_t>(byte & 0x7F)) * multiplier;
            multiplier *= 128;
            bytes_consumed = i + 1;
            if ((byte & 0x80) == 0)
                return value;
        }
        return std::nullopt;
    }

    std::optional<std::string> MqttCodec::read_utf8_string(const uint8_t * data, size_t len, size_t & offset)
    {
        if (offset + 2 > len)
            return std::nullopt;
        uint16_t slen = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
        offset += 2;
        if (offset + slen > len)
            return std::nullopt;
        std::string result(reinterpret_cast<const char *>(data + offset), slen);
        offset += slen;
        return result;
    }

    void MqttCodec::write_utf8_string(ByteBuffer & buf, const std::string & str)
    {
        buf.append_u16(static_cast<uint16_t>(str.size()));
        buf.append(str.data(), str.size());
    }

    std::optional<std::vector<uint8_t> > MqttCodec::read_binary_data(const uint8_t * data, size_t len, size_t & offset)
    {
        if (offset + 2 > len)
            return std::nullopt;
        uint16_t dlen = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
        offset += 2;
        if (offset + dlen > len)
            return std::nullopt;
        std::vector<uint8_t> result(data + offset, data + offset + dlen);
        offset += dlen;
        return result;
    }

    void MqttCodec::write_binary_data(ByteBuffer & buf, const std::vector<uint8_t> & data)
    {
        buf.append_u16(static_cast<uint16_t>(data.size()));
        if (!data.empty())
            buf.append(data.data(), data.size());
    }

    ByteBuffer MqttCodec::build_fixed_header(PacketType type, uint8_t flags, size_t remaining_length)
    {
        uint8_t first_byte = (static_cast<uint8_t>(type) << 4) | (flags & 0x0F);
        uint8_t vbi[MQTT_VARIABLE_BYTE_INT_MAX_SIZE];
        size_t vbi_len = encode_remaining_length(static_cast<uint32_t>(remaining_length), vbi);
        ByteBuffer buf(1 + vbi_len);
        buf.append_u8(first_byte);
        buf.append(vbi, vbi_len);
        return buf;
    }

    std::optional<std::pair<PacketType, size_t> > MqttCodec::try_decode(const uint8_t * data, size_t len)
    {
        if (len < 2)
            return std::nullopt;

        uint8_t first_byte = data[0];
        uint8_t type_val = first_byte >> 4;
        if (type_val < static_cast<uint8_t>(PacketType::CONNECT) ||
            type_val > static_cast<uint8_t>(PacketType::AUTH))
            return std::nullopt;

        size_t vbi_bytes = 0;
        auto remaining = decode_remaining_length(data + 1, len - 1, vbi_bytes);
        if (!remaining)
            return std::nullopt;

        size_t total = 1 + vbi_bytes + *remaining;
        return std::make_pair(static_cast<PacketType>(type_val), total);
    }

    std::optional<MqttConnectPacket> MqttCodec::decode_connect(const uint8_t * data, size_t len)
    {
        size_t offset = 0;

        auto proto_name = read_utf8_string(data, len, offset);
        if (!proto_name || *proto_name != MQTT_PROTOCOL_NAME_V5)
            return std::nullopt;

        if (offset + 1 > len)
            return std::nullopt;
        ProtocolLevel level = static_cast<ProtocolLevel>(data[offset++]);
        if (level != ProtocolLevel::V3_1_1 && level != ProtocolLevel::V5_0)
            return std::nullopt;

        if (offset + 1 > len)
            return std::nullopt;
        uint8_t connect_flags = data[offset++];

        if (offset + 2 > len)
            return std::nullopt;
        uint16_t keep_alive = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
        offset += 2;

        MqttConnectPacket pkt;
        pkt.protocol_level = level;
        pkt.connect_flags = connect_flags;
        pkt.keep_alive = keep_alive;

        if (level == ProtocolLevel::V5_0) {
            if (decode_properties(data, len, offset, pkt.properties) == 0)
                return std::nullopt;
        }

        auto client_id = read_utf8_string(data, len, offset);
        if (!client_id)
            return std::nullopt;
        pkt.client_id = std::move(*client_id);

        if (connect_flags & MQTT_CONNECT_FLAG_WILL_FLAG) {
            if (level == ProtocolLevel::V5_0) {
                if (decode_properties(data, len, offset, pkt.will_properties) == 0)
                    return std::nullopt;
            }
            auto will_topic = read_utf8_string(data, len, offset);
            if (!will_topic)
                return std::nullopt;
            pkt.will_topic = std::move(*will_topic);
            auto will_payload = read_binary_data(data, len, offset);
            if (!will_payload)
                return std::nullopt;
            pkt.will_payload = std::move(*will_payload);
        }

        if (connect_flags & MQTT_CONNECT_FLAG_USERNAME) {
            auto username = read_utf8_string(data, len, offset);
            if (!username)
                return std::nullopt;
            pkt.username = std::move(*username);
        }

        if (connect_flags & MQTT_CONNECT_FLAG_PASSWORD) {
            auto password = read_utf8_string(data, len, offset);
            if (!password)
                return std::nullopt;
            pkt.password = std::move(*password);
        }

        return pkt;
    }

    std::optional<MqttPublishPacket> MqttCodec::decode_publish(const uint8_t * data, size_t len, uint8_t flags)
    {
        size_t offset = 0;

        MqttPublishPacket pkt;
        pkt.dup = (flags & MQTT_PUBLISH_FLAG_DUP) ? 1 : 0;
        pkt.qos = static_cast<QoS>((flags >> MQTT_PUBLISH_FLAG_QOS_SHIFT) & 0x03);
        pkt.retain = flags & MQTT_PUBLISH_FLAG_RETAIN;

        auto topic = read_utf8_string(data, len, offset);
        if (!topic)
            return std::nullopt;
        pkt.topic = std::move(*topic);

        if (pkt.qos != QoS::AT_MOST_ONCE) {
            if (offset + 2 > len)
                return std::nullopt;
            pkt.packet_id = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
            offset += 2;
        }

        if (offset < len) {
            pkt.payload.assign(data + offset, data + len);
        }

        return pkt;
    }

    std::optional<MqttSubscribePacket> MqttCodec::decode_subscribe(const uint8_t * data, size_t len, ProtocolLevel level)
    {
        size_t offset = 0;

        if (offset + 2 > len)
            return std::nullopt;
        uint16_t packet_id = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
        offset += 2;

        MqttSubscribePacket pkt;
        pkt.packet_id = packet_id;

        if (level == ProtocolLevel::V5_0) {
            if (decode_properties(data, len, offset, pkt.properties) == 0)
                return std::nullopt;
        }

        while (offset < len) {
            MqttSubOption sub;
            auto topic_filter = read_utf8_string(data, len, offset);
            if (!topic_filter)
                return std::nullopt;
            sub.topic_filter = std::move(*topic_filter);

            if (offset + 1 > len)
                return std::nullopt;
            uint8_t options = data[offset++];
            if (level == ProtocolLevel::V5_0) {
                sub.maximum_qos = static_cast<QoS>(options & 0x03);
                sub.no_local = (options >> 2) & 0x01;
                sub.retain_as_published = (options >> 3) & 0x01;
                sub.retain_handling = (options >> 4) & 0x03;
            } else {
                sub.maximum_qos = static_cast<QoS>(options & 0x03);
            }

            pkt.subscriptions.push_back(std::move(sub));
        }

        return pkt;
    }

    std::optional<MqttUnsubscribePacket> MqttCodec::decode_unsubscribe(const uint8_t * data, size_t len, ProtocolLevel level)
    {
        size_t offset = 0;

        if (offset + 2 > len)
            return std::nullopt;
        uint16_t packet_id = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
        offset += 2;

        MqttUnsubscribePacket pkt;
        pkt.packet_id = packet_id;

        if (level == ProtocolLevel::V5_0) {
            if (decode_properties(data, len, offset, pkt.properties) == 0)
                return std::nullopt;
        }

        while (offset < len) {
            auto topic_filter = read_utf8_string(data, len, offset);
            if (!topic_filter)
                return std::nullopt;
            pkt.topic_filters.push_back(std::move(*topic_filter));
        }

        return pkt;
    }

    std::optional<uint16_t> MqttCodec::decode_packet_id(const uint8_t * data, size_t len)
    {
        if (len < 2)
            return std::nullopt;
        return (static_cast<uint16_t>(data[0]) << 8) | data[1];
    }

    static std::optional<MqttPubackPacket> decode_simple_ack(const uint8_t * data, size_t len, ProtocolLevel level)
    {
        if (len < 2)
            return std::nullopt;

        MqttPubackPacket pkt;
        pkt.packet_id = (static_cast<uint16_t>(data[0]) << 8) | data[1];

        if (level == ProtocolLevel::V5_0 && len > 2) {
            pkt.reason_code = data[2];
            size_t offset = 3;
            MqttProperties props;
            if (decode_properties(data, len, offset, props) == 0)
                return std::nullopt;
            pkt.properties = std::move(props);
        }

        return pkt;
    }

    std::optional<MqttPubackPacket> MqttCodec::decode_puback(const uint8_t * data, size_t len, ProtocolLevel level)
    {
        return decode_simple_ack(data, len, level);
    }

    std::optional<MqttPubrecPacket> MqttCodec::decode_pubrec(const uint8_t * data, size_t len, ProtocolLevel level)
    {
        if (len < 2)
            return std::nullopt;

        MqttPubrecPacket pkt;
        pkt.packet_id = (static_cast<uint16_t>(data[0]) << 8) | data[1];

        if (level == ProtocolLevel::V5_0 && len > 2) {
            pkt.reason_code = data[2];
            size_t offset = 3;
            MqttProperties props;
            if (decode_properties(data, len, offset, props) == 0)
                return std::nullopt;
            pkt.properties = std::move(props);
        }

        return pkt;
    }

    std::optional<MqttPubrelPacket> MqttCodec::decode_pubrel(const uint8_t * data, size_t len, ProtocolLevel level)
    {
        if (len < 2)
            return std::nullopt;

        MqttPubrelPacket pkt;
        pkt.packet_id = (static_cast<uint16_t>(data[0]) << 8) | data[1];

        if (level == ProtocolLevel::V5_0 && len > 2) {
            pkt.reason_code = data[2];
            size_t offset = 3;
            MqttProperties props;
            if (decode_properties(data, len, offset, props) == 0)
                return std::nullopt;
            pkt.properties = std::move(props);
        }

        return pkt;
    }

    std::optional<MqttPubcompPacket> MqttCodec::decode_pubcomp(const uint8_t * data, size_t len, ProtocolLevel level)
    {
        if (len < 2)
            return std::nullopt;

        MqttPubcompPacket pkt;
        pkt.packet_id = (static_cast<uint16_t>(data[0]) << 8) | data[1];

        if (level == ProtocolLevel::V5_0 && len > 2) {
            pkt.reason_code = data[2];
            size_t offset = 3;
            MqttProperties props;
            if (decode_properties(data, len, offset, props) == 0)
                return std::nullopt;
            pkt.properties = std::move(props);
        }

        return pkt;
    }

    std::optional<MqttDisconnectPacket> MqttCodec::decode_disconnect(const uint8_t * data, size_t len)
    {
        MqttDisconnectPacket pkt;

        if (len >= 1) {
            pkt.reason_code = data[0];
            if (len > 1) {
                size_t offset = 1;
                if (decode_properties(data, len, offset, pkt.properties) == 0)
                    return std::nullopt;
            }
        }

        return pkt;
    }

    std::optional<MqttAuthPacket> MqttCodec::decode_auth(const uint8_t * data, size_t len)
    {
        MqttAuthPacket pkt;

        if (len >= 1) {
            pkt.reason_code = data[0];
            if (len > 1) {
                size_t offset = 1;
                if (decode_properties(data, len, offset, pkt.properties) == 0)
                    return std::nullopt;
            }
        }

        return pkt;
    }

    ByteBuffer MqttCodec::encode_connack(const MqttConnackPacket & pkt, ProtocolLevel level)
    {
        ByteBuffer body(64);
        body.append_u8(pkt.session_present & 0x01);
        body.append_u8(pkt.reason_code);

        if (level == ProtocolLevel::V5_0) {
            encode_properties(body, pkt.properties);
        }

        ByteBuffer result = build_fixed_header(PacketType::CONNACK, 0, body.write_offset());
        result.append(body.readable_span());
        return result;
    }

    ByteBuffer MqttCodec::encode_publish(const MqttPublishPacket & pkt, ProtocolLevel level)
    {
        ByteBuffer body(256);
        write_utf8_string(body, pkt.topic);

        if (pkt.qos != QoS::AT_MOST_ONCE) {
            body.append_u16(pkt.packet_id.value_or(0));
        }

        if (level == ProtocolLevel::V5_0) {
            encode_properties(body, pkt.properties);
        }

        if (!pkt.payload.empty()) {
            body.append(pkt.payload.data(), pkt.payload.size());
        }

        uint8_t flags = (pkt.dup << 3) |
                        (static_cast<uint8_t>(pkt.qos) << MQTT_PUBLISH_FLAG_QOS_SHIFT) |
                        (pkt.retain & MQTT_PUBLISH_FLAG_RETAIN);

        ByteBuffer result = build_fixed_header(PacketType::PUBLISH, flags, body.write_offset());
        result.append(body.readable_span());
        return result;
    }

    ByteBuffer MqttCodec::encode_suback(uint16_t packet_id, const std::vector<uint8_t> & reason_codes, ProtocolLevel level, const MqttProperties & props)
    {
        ByteBuffer body(64 + reason_codes.size());
        body.append_u16(packet_id);

        if (level == ProtocolLevel::V5_0) {
            encode_properties(body, props);
        }

        for (uint8_t rc : reason_codes) {
            body.append_u8(rc);
        }

        ByteBuffer result = build_fixed_header(PacketType::SUBACK, 0, body.write_offset());
        result.append(body.readable_span());
        return result;
    }

    static ByteBuffer encode_simple_ack(PacketType type, uint8_t fixed_flags,
                                        uint16_t packet_id, uint8_t reason_code,
                                        ProtocolLevel level, const MqttProperties & props)
    {
        ByteBuffer body(16);
        body.append_u16(packet_id);

        if (level == ProtocolLevel::V5_0) {
            body.append_u8(reason_code);
            encode_properties(body, props);
        }

        ByteBuffer result = MqttCodec::build_fixed_header(type, fixed_flags, body.write_offset());
        result.append(body.readable_span());
        return result;
    }

    ByteBuffer MqttCodec::encode_puback(uint16_t packet_id, uint8_t reason_code, ProtocolLevel level, const MqttProperties & props)
    {
        return encode_simple_ack(PacketType::PUBACK, 0, packet_id, reason_code, level, props);
    }

    ByteBuffer MqttCodec::encode_pubrec(uint16_t packet_id, uint8_t reason_code, ProtocolLevel level, const MqttProperties & props)
    {
        return encode_simple_ack(PacketType::PUBREC, 0, packet_id, reason_code, level, props);
    }

    ByteBuffer MqttCodec::encode_pubrel(uint16_t packet_id, uint8_t reason_code, ProtocolLevel level, const MqttProperties & props)
    {
        return encode_simple_ack(PacketType::PUBREL, 0x02, packet_id, reason_code, level, props);
    }

    ByteBuffer MqttCodec::encode_pubcomp(uint16_t packet_id, uint8_t reason_code, ProtocolLevel level, const MqttProperties & props)
    {
        return encode_simple_ack(PacketType::PUBCOMP, 0, packet_id, reason_code, level, props);
    }

    ByteBuffer MqttCodec::encode_unsuback(uint16_t packet_id, const std::vector<uint8_t> & reason_codes, ProtocolLevel level, const MqttProperties & props)
    {
        ByteBuffer body(64 + reason_codes.size());
        body.append_u16(packet_id);

        if (level == ProtocolLevel::V5_0) {
            encode_properties(body, props);
            for (uint8_t rc : reason_codes) {
                body.append_u8(rc);
            }
        }

        ByteBuffer result = build_fixed_header(PacketType::UNSUBACK, 0, body.write_offset());
        result.append(body.readable_span());
        return result;
    }

    ByteBuffer MqttCodec::encode_pingresp()
    {
        ByteBuffer buf(2);
        buf.append_u8(0xD0);
        buf.append_u8(0x00);
        return buf;
    }

    ByteBuffer MqttCodec::encode_disconnect(uint8_t reason_code, ProtocolLevel level, const MqttProperties & props)
    {
        if (level == ProtocolLevel::V3_1_1) {
            ByteBuffer buf(2);
            buf.append_u8(0xE0);
            buf.append_u8(0x00);
            return buf;
        }

        ByteBuffer body(16);
        body.append_u8(reason_code);
        encode_properties(body, props);

        ByteBuffer result = build_fixed_header(PacketType::DISCONNECT, 0, body.write_offset());
        result.append(body.readable_span());
        return result;
    }
}
