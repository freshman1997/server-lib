#include "mqtt_properties.h"
#include <cstring>

namespace yuan::net::mqtt
{
    bool MqttProperties::has_any() const
    {
        return payload_format_indicator.has_value() ||
               message_expiry_interval.has_value() ||
               content_type.has_value() ||
               response_topic.has_value() ||
               correlation_data.has_value() ||
               subscription_identifier.has_value() ||
               session_expiry_interval.has_value() ||
               assigned_client_identifier.has_value() ||
               server_keep_alive.has_value() ||
               authentication_method.has_value() ||
               authentication_data.has_value() ||
               request_problem_information.has_value() ||
               will_delay_interval.has_value() ||
               request_response_information.has_value() ||
               response_information.has_value() ||
               server_reference.has_value() ||
               reason_string.has_value() ||
               receive_maximum.has_value() ||
               topic_alias_maximum.has_value() ||
               topic_alias.has_value() ||
               maximum_qos.has_value() ||
               retain_available.has_value() ||
               maximum_packet_size.has_value() ||
               wildcard_subscription_available.has_value() ||
               subscription_identifier_available.has_value() ||
               shared_subscription_available.has_value() ||
               !user_properties.empty();
    }

    void MqttProperties::clear()
    {
        payload_format_indicator.reset();
        message_expiry_interval.reset();
        content_type.reset();
        response_topic.reset();
        correlation_data.reset();
        subscription_identifier.reset();
        session_expiry_interval.reset();
        assigned_client_identifier.reset();
        server_keep_alive.reset();
        authentication_method.reset();
        authentication_data.reset();
        request_problem_information.reset();
        will_delay_interval.reset();
        request_response_information.reset();
        response_information.reset();
        server_reference.reset();
        reason_string.reset();
        receive_maximum.reset();
        topic_alias_maximum.reset();
        topic_alias.reset();
        maximum_qos.reset();
        retain_available.reset();
        maximum_packet_size.reset();
        wildcard_subscription_available.reset();
        subscription_identifier_available.reset();
        shared_subscription_available.reset();
        user_properties.clear();
    }

    static size_t variable_byte_int_size(uint32_t value)
    {
        if (value < 128)
            return 1;
        if (value < 16384)
            return 2;
        if (value < 2097152)
            return 3;
        return 4;
    }

    static void encode_variable_byte_int(ByteBuffer & buf, uint32_t value)
    {
        do {
            uint8_t byte = value & 0x7F;
            value >>= 7;
            if (value > 0)
                byte |= 0x80;
            buf.append_u8(byte);
        } while (value > 0);
    }

    static void write_utf8_string(ByteBuffer & buf, const std::string & str)
    {
        buf.append_u16(static_cast<uint16_t>(str.size()));
        buf.append(str.data(), str.size());
    }

    static void write_binary_data(ByteBuffer & buf, const std::vector<uint8_t> & data)
    {
        buf.append_u16(static_cast<uint16_t>(data.size()));
        if (!data.empty()) {
            buf.append(data.data(), data.size());
        }
    }

    size_t MqttProperties::encoded_size() const
    {
        size_t total = 0;
        auto add_byte = [&](PropertyId id, uint8_t) { total += 2; };
        auto add_u16 = [&](PropertyId id, uint16_t) { total += 3; };
        auto add_u32 = [&](PropertyId id, uint32_t v) { total += 1 + variable_byte_int_size(static_cast<uint32_t>(id)) + 4; };

        if (payload_format_indicator)
            total += 2;
        if (message_expiry_interval)
            total += 5;
        if (content_type)
            total += 1 + 2 + content_type->size();
        if (response_topic)
            total += 1 + 2 + response_topic->size();
        if (correlation_data)
            total += 1 + 2 + correlation_data->size();
        if (subscription_identifier)
            total += 1 + variable_byte_int_size(*subscription_identifier);
        if (session_expiry_interval)
            total += 5;
        if (assigned_client_identifier)
            total += 1 + 2 + assigned_client_identifier->size();
        if (server_keep_alive)
            total += 3;
        if (authentication_method)
            total += 1 + 2 + authentication_method->size();
        if (authentication_data)
            total += 1 + 2 + authentication_data->size();
        if (request_problem_information)
            total += 2;
        if (will_delay_interval)
            total += 5;
        if (request_response_information)
            total += 2;
        if (response_information)
            total += 1 + 2 + response_information->size();
        if (server_reference)
            total += 1 + 2 + server_reference->size();
        if (reason_string)
            total += 1 + 2 + reason_string->size();
        if (receive_maximum)
            total += 3;
        if (topic_alias_maximum)
            total += 3;
        if (topic_alias)
            total += 3;
        if (maximum_qos)
            total += 2;
        if (retain_available)
            total += 2;
        if (maximum_packet_size)
            total += 5;
        if (wildcard_subscription_available)
            total += 2;
        if (subscription_identifier_available)
            total += 2;
        if (shared_subscription_available)
            total += 2;
        for (const auto &up : user_properties) {
            total += 1 + 2 + up.key.size() + 2 + up.value.size();
        }
        return total;
    }

    void encode_properties(ByteBuffer & buf, const MqttProperties & props)
    {
        if (!props.has_any()) {
            encode_variable_byte_int(buf, 0);
            return;
        }

        ByteBuffer prop_buf(props.encoded_size() + 4);

        auto write_byte_prop = [&](PropertyId id, uint8_t val) {
            prop_buf.append_u8(static_cast<uint8_t>(id));
            prop_buf.append_u8(val);
        };
        auto write_u16_prop = [&](PropertyId id, uint16_t val) {
            prop_buf.append_u8(static_cast<uint8_t>(id));
            prop_buf.append_u16(val);
        };
        auto write_u32_prop = [&](PropertyId id, uint32_t val) {
            prop_buf.append_u8(static_cast<uint8_t>(id));
            prop_buf.append_u32(val);
        };

        if (props.payload_format_indicator)
            write_byte_prop(PropertyId::PAYLOAD_FORMAT_INDICICATOR, *props.payload_format_indicator);
        if (props.message_expiry_interval)
            write_u32_prop(PropertyId::MESSAGE_EXPIRY_INTERVAL, *props.message_expiry_interval);
        if (props.content_type) {
            prop_buf.append_u8(static_cast<uint8_t>(PropertyId::CONTENT_TYPE));
            write_utf8_string(prop_buf, *props.content_type);
        }
        if (props.response_topic) {
            prop_buf.append_u8(static_cast<uint8_t>(PropertyId::RESPONSE_TOPIC));
            write_utf8_string(prop_buf, *props.response_topic);
        }
        if (props.correlation_data) {
            prop_buf.append_u8(static_cast<uint8_t>(PropertyId::CORRELATION_DATA));
            write_binary_data(prop_buf, *props.correlation_data);
        }
        if (props.subscription_identifier) {
            prop_buf.append_u8(static_cast<uint8_t>(PropertyId::SUBSCRIPTION_IDENTIFIER));
            encode_variable_byte_int(prop_buf, *props.subscription_identifier);
        }
        if (props.session_expiry_interval)
            write_u32_prop(PropertyId::SESSION_EXPIRY_INTERVAL, *props.session_expiry_interval);
        if (props.assigned_client_identifier) {
            prop_buf.append_u8(static_cast<uint8_t>(PropertyId::ASSIGNED_CLIENT_IDENTIFIER));
            write_utf8_string(prop_buf, *props.assigned_client_identifier);
        }
        if (props.server_keep_alive)
            write_u16_prop(PropertyId::SERVER_KEEP_ALIVE, *props.server_keep_alive);
        if (props.authentication_method) {
            prop_buf.append_u8(static_cast<uint8_t>(PropertyId::AUTHENTICATION_METHOD));
            write_utf8_string(prop_buf, *props.authentication_method);
        }
        if (props.authentication_data) {
            prop_buf.append_u8(static_cast<uint8_t>(PropertyId::AUTHENTICATION_DATA));
            write_binary_data(prop_buf, *props.authentication_data);
        }
        if (props.request_problem_information)
            write_byte_prop(PropertyId::REQUEST_PROBLEM_INFORMATION, *props.request_problem_information);
        if (props.will_delay_interval)
            write_u32_prop(PropertyId::WILL_DELAY_INTERVAL, *props.will_delay_interval);
        if (props.request_response_information)
            write_byte_prop(PropertyId::REQUEST_RESPONSE_INFORMATION, *props.request_response_information);
        if (props.response_information) {
            prop_buf.append_u8(static_cast<uint8_t>(PropertyId::RESPONSE_INFORMATION));
            write_utf8_string(prop_buf, *props.response_information);
        }
        if (props.server_reference) {
            prop_buf.append_u8(static_cast<uint8_t>(PropertyId::SERVER_REFERENCE));
            write_utf8_string(prop_buf, *props.server_reference);
        }
        if (props.reason_string) {
            prop_buf.append_u8(static_cast<uint8_t>(PropertyId::REASON_STRING));
            write_utf8_string(prop_buf, *props.reason_string);
        }
        if (props.receive_maximum)
            write_u16_prop(PropertyId::RECEIVE_MAXIMUM, *props.receive_maximum);
        if (props.topic_alias_maximum)
            write_u16_prop(PropertyId::TOPIC_ALIAS_MAXIMUM, *props.topic_alias_maximum);
        if (props.topic_alias)
            write_u16_prop(PropertyId::TOPIC_ALIAS, *props.topic_alias);
        if (props.maximum_qos)
            write_byte_prop(PropertyId::MAXIMUM_QOS, *props.maximum_qos);
        if (props.retain_available)
            write_byte_prop(PropertyId::RETAIN_AVAILABLE, *props.retain_available);
        if (props.maximum_packet_size)
            write_u32_prop(PropertyId::MAXIMUM_PACKET_SIZE, *props.maximum_packet_size);
        if (props.wildcard_subscription_available)
            write_byte_prop(PropertyId::WILDCARD_SUBSCRIPTION_AVAILABLE, *props.wildcard_subscription_available);
        if (props.subscription_identifier_available)
            write_byte_prop(PropertyId::SUBSCRIPTION_IDENTIFIER_AVAILABLE, *props.subscription_identifier_available);
        if (props.shared_subscription_available)
            write_byte_prop(PropertyId::SHARED_SUBSCRIPTION_AVAILABLE, *props.shared_subscription_available);
        for (const auto &up : props.user_properties) {
            prop_buf.append_u8(static_cast<uint8_t>(PropertyId::USER_PROPERTY));
            write_utf8_string(prop_buf, up.key);
            write_utf8_string(prop_buf, up.value);
        }

        encode_variable_byte_int(buf, static_cast<uint32_t>(prop_buf.write_offset()));
        buf.append(prop_buf.readable_span());
    }

    static std::optional<uint32_t> decode_variable_byte_int(const uint8_t * data, size_t len, size_t & offset)
    {
        uint32_t value = 0;
        uint32_t multiplier = 1;
        size_t consumed = 0;
        for (; consumed < 4 && offset + consumed < len; ++consumed) {
            uint8_t byte = data[offset + consumed];
            value += (byte & 0x7F) * multiplier;
            multiplier *= 128;
            if ((byte & 0x80) == 0) {
                offset += consumed + 1;
                return value;
            }
        }
        return std::nullopt;
    }

    static std::optional<std::string> read_utf8_string(const uint8_t * data, size_t len, size_t & offset)
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

    static std::optional<std::vector<uint8_t> > read_binary_data(const uint8_t * data, size_t len, size_t & offset)
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

    size_t decode_properties(const uint8_t * data, size_t len, size_t & offset, MqttProperties & props)
    {
        if (offset >= len)
            return 0;

        size_t prop_start = offset;
        auto prop_len = decode_variable_byte_int(data, len, offset);
        if (!prop_len)
            return 0;

        size_t prop_end = offset + *prop_len;
        if (prop_end > len)
            return 0;

        while (offset < prop_end) {
            uint8_t id = data[offset++];
            switch (static_cast<PropertyId>(id)) {
            case PropertyId::PAYLOAD_FORMAT_INDICICATOR:
                if (offset + 1 > prop_end)
                    return 0;
                props.payload_format_indicator = data[offset++];
                break;
            case PropertyId::MESSAGE_EXPIRY_INTERVAL:
                if (offset + 4 > prop_end)
                    return 0;
                props.message_expiry_interval = (static_cast<uint32_t>(data[offset]) << 24) |
                                                (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                                (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                                data[offset + 3];
                offset += 4;
                break;
            case PropertyId::CONTENT_TYPE: {
                auto s = read_utf8_string(data, prop_end, offset);
                if (s)
                    props.content_type = std::move(*s);
                else
                    return 0;
                break;
            }
            case PropertyId::RESPONSE_TOPIC: {
                auto s = read_utf8_string(data, prop_end, offset);
                if (s)
                    props.response_topic = std::move(*s);
                else
                    return 0;
                break;
            }
            case PropertyId::CORRELATION_DATA: {
                auto d = read_binary_data(data, prop_end, offset);
                if (d)
                    props.correlation_data = std::move(*d);
                else
                    return 0;
                break;
            }
            case PropertyId::SUBSCRIPTION_IDENTIFIER: {
                auto v = decode_variable_byte_int(data, prop_end, offset);
                if (v)
                    props.subscription_identifier = *v;
                else
                    return 0;
                break;
            }
            case PropertyId::SESSION_EXPIRY_INTERVAL:
                if (offset + 4 > prop_end)
                    return 0;
                props.session_expiry_interval = (static_cast<uint32_t>(data[offset]) << 24) |
                                                (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                                (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                                data[offset + 3];
                offset += 4;
                break;
            case PropertyId::ASSIGNED_CLIENT_IDENTIFIER: {
                auto s = read_utf8_string(data, prop_end, offset);
                if (s)
                    props.assigned_client_identifier = std::move(*s);
                else
                    return 0;
                break;
            }
            case PropertyId::SERVER_KEEP_ALIVE:
                if (offset + 2 > prop_end)
                    return 0;
                props.server_keep_alive = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
                offset += 2;
                break;
            case PropertyId::AUTHENTICATION_METHOD: {
                auto s = read_utf8_string(data, prop_end, offset);
                if (s)
                    props.authentication_method = std::move(*s);
                else
                    return 0;
                break;
            }
            case PropertyId::AUTHENTICATION_DATA: {
                auto d = read_binary_data(data, prop_end, offset);
                if (d)
                    props.authentication_data = std::move(*d);
                else
                    return 0;
                break;
            }
            case PropertyId::REQUEST_PROBLEM_INFORMATION:
                if (offset + 1 > prop_end)
                    return 0;
                props.request_problem_information = data[offset++];
                break;
            case PropertyId::WILL_DELAY_INTERVAL:
                if (offset + 4 > prop_end)
                    return 0;
                props.will_delay_interval = (static_cast<uint32_t>(data[offset]) << 24) |
                                            (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                            (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                            data[offset + 3];
                offset += 4;
                break;
            case PropertyId::REQUEST_RESPONSE_INFORMATION:
                if (offset + 1 > prop_end)
                    return 0;
                props.request_response_information = data[offset++];
                break;
            case PropertyId::RESPONSE_INFORMATION: {
                auto s = read_utf8_string(data, prop_end, offset);
                if (s)
                    props.response_information = std::move(*s);
                else
                    return 0;
                break;
            }
            case PropertyId::SERVER_REFERENCE: {
                auto s = read_utf8_string(data, prop_end, offset);
                if (s)
                    props.server_reference = std::move(*s);
                else
                    return 0;
                break;
            }
            case PropertyId::REASON_STRING: {
                auto s = read_utf8_string(data, prop_end, offset);
                if (s)
                    props.reason_string = std::move(*s);
                else
                    return 0;
                break;
            }
            case PropertyId::RECEIVE_MAXIMUM:
                if (offset + 2 > prop_end)
                    return 0;
                props.receive_maximum = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
                offset += 2;
                break;
            case PropertyId::TOPIC_ALIAS_MAXIMUM:
                if (offset + 2 > prop_end)
                    return 0;
                props.topic_alias_maximum = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
                offset += 2;
                break;
            case PropertyId::TOPIC_ALIAS:
                if (offset + 2 > prop_end)
                    return 0;
                props.topic_alias = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
                offset += 2;
                break;
            case PropertyId::MAXIMUM_QOS:
                if (offset + 1 > prop_end)
                    return 0;
                props.maximum_qos = data[offset++];
                break;
            case PropertyId::RETAIN_AVAILABLE:
                if (offset + 1 > prop_end)
                    return 0;
                props.retain_available = data[offset++];
                break;
            case PropertyId::MAXIMUM_PACKET_SIZE:
                if (offset + 4 > prop_end)
                    return 0;
                props.maximum_packet_size = (static_cast<uint32_t>(data[offset]) << 24) |
                                            (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                            (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                            data[offset + 3];
                offset += 4;
                break;
            case PropertyId::WILDCARD_SUBSCRIPTION_AVAILABLE:
                if (offset + 1 > prop_end)
                    return 0;
                props.wildcard_subscription_available = data[offset++];
                break;
            case PropertyId::SUBSCRIPTION_IDENTIFIER_AVAILABLE:
                if (offset + 1 > prop_end)
                    return 0;
                props.subscription_identifier_available = data[offset++];
                break;
            case PropertyId::SHARED_SUBSCRIPTION_AVAILABLE:
                if (offset + 1 > prop_end)
                    return 0;
                props.shared_subscription_available = data[offset++];
                break;
            case PropertyId::USER_PROPERTY: {
                auto k = read_utf8_string(data, prop_end, offset);
                if (!k)
                    return 0;
                auto v = read_utf8_string(data, prop_end, offset);
                if (!v)
                    return 0;
                props.user_properties.push_back({ std::move(*k), std::move(*v) });
                break;
            }
            default:
                return 0;
            }
        }

        return prop_end - prop_start;
    }
}
