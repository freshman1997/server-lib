#include "mqtt_dispatcher.h"
#include "mqtt_session.h"
#include "net/connection/tcp_connection.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>

namespace yuan::net::mqtt
{
    namespace
    {
        void close_session_connection(MqttSession & session)
        {
            if (auto conn = session.connection()) {
                conn->close();
            }
        }

        ByteBuffer protocol_error_disconnect(const MqttSession & session, DisconnectReason reason)
        {
            if (session.protocol_level() != ProtocolLevel::V5_0)
                return {};
            return MqttCodec::encode_disconnect(static_cast<uint8_t>(reason), session.protocol_level(), {});
        }

        bool is_valid_publish_flags(uint8_t flags)
        {
            const uint8_t qos_bits = static_cast<uint8_t>((flags >> MQTT_PUBLISH_FLAG_QOS_SHIFT) & 0x03);
            if (qos_bits == 0x03)
                return false;
            if (qos_bits == 0 && (flags & MQTT_PUBLISH_FLAG_DUP))
                return false;
            return true;
        }

        bool is_valid_packet_flags(PacketType type, uint8_t flags)
        {
            switch (type) {
            case PacketType::PUBREL:
            case PacketType::SUBSCRIBE:
            case PacketType::UNSUBSCRIBE:
                return flags == 0x02;
            case PacketType::PUBLISH:
                return is_valid_publish_flags(flags);
            default:
                return flags == 0x00;
            }
        }

        std::optional<std::string> extract_shared_group(const std::string & filter)
        {
            if (!MqttTopicTree::is_shared_subscription(filter))
                return std::nullopt;
            const std::string group = MqttTopicTree::shared_group(filter);
            if (group.empty())
                return std::nullopt;
            return group;
        }

        std::vector<MqttSubscription> apply_shared_subscription_policy(const std::vector<MqttSubscription> & matches)
        {
            if (matches.empty())
                return {};

            std::vector<MqttSubscription> result;
            result.reserve(matches.size());
            std::map<std::string, std::vector<const MqttSubscription *> > shared_groups;

            for (const auto &sub : matches) {
                if (sub.shared_group.empty()) {
                    result.push_back(sub);
                    continue;
                }
                shared_groups[sub.shared_group].push_back(&sub);
            }

            for (const auto &entry : shared_groups) {
                const auto &subs = entry.second;
                if (subs.empty())
                    continue;

                uint64_t score = 0;
                for (char ch : entry.first) {
                    score = (score * 131ULL) + static_cast<uint8_t>(ch);
                }
                const size_t index = static_cast<size_t>(score % subs.size());
                result.push_back(*subs[index]);
            }

            return result;
        }

        bool parse_remaining_length(const uint8_t * data, size_t len, size_t & header_len, size_t & body_len)
        {
            if (!data || len < 2)
                return false;

            uint32_t value = 0;
            uint32_t multiplier = 1;
            size_t consumed = 0;
            for (; consumed < MQTT_VARIABLE_BYTE_INT_MAX_SIZE && (1 + consumed) < len; ++consumed) {
                uint8_t byte = data[1 + consumed];
                value += (static_cast<uint32_t>(byte & 0x7F) * multiplier);
                multiplier *= 128;
                if ((byte & 0x80) == 0)
                    break;
            }

            if (consumed >= MQTT_VARIABLE_BYTE_INT_MAX_SIZE)
                return false;

            if ((data[1 + consumed] & 0x80) != 0)
                return false;

            header_len = 1 + consumed + 1;
            body_len = value;
            return header_len + body_len == len;
        }
    }

    MqttDispatcher::MqttDispatcher(const MqttServerConfig & config,
                                   MqttSessionManager & session_mgr,
                                   MqttTopicTree & topic_tree,
                                   MqttRetainedStore & retained_store,
                                   MqttHandler * handler)
        : config_(config), session_mgr_(session_mgr), topic_tree_(topic_tree), retained_store_(retained_store), handler_(handler)
    {
    }

    ByteBuffer MqttDispatcher::dispatch(MqttSession & session, const uint8_t * data, size_t len)
    {
        if (len == 0)
            return {};

        uint8_t first_byte = data[0];
        auto pkt_type = static_cast<PacketType>((first_byte >> 4) & 0x0F);
        uint8_t flags = first_byte & 0x0F;

        if (session.state() != MqttSessionState::connected && pkt_type != PacketType::CONNECT) {
            return protocol_error_disconnect(session, DisconnectReason::PROTOCOL_ERROR);
        }

        if (session.state() == MqttSessionState::connected && pkt_type == PacketType::CONNECT) {
            session.set_state(MqttSessionState::disconnecting);
            close_session_connection(session);
            return protocol_error_disconnect(session, DisconnectReason::PROTOCOL_ERROR);
        }

        if (!is_valid_packet_flags(pkt_type, flags)) {
            session.set_state(MqttSessionState::disconnecting);
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);
        }

        size_t fixed_header_len = 0;
        size_t body_len = 0;
        if (!parse_remaining_length(data, len, fixed_header_len, body_len))
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);

        const uint8_t * body = data + fixed_header_len;

        switch (pkt_type) {
        case PacketType::CONNECT:
            return handle_connect(session, body, body_len);
        case PacketType::PUBLISH:
            return handle_publish(session, body, body_len, flags);
        case PacketType::PUBACK:
            return handle_puback(session, body, body_len);
        case PacketType::PUBREC:
            return handle_pubrec(session, body, body_len);
        case PacketType::PUBREL:
            return handle_pubrel(session, body, body_len);
        case PacketType::PUBCOMP:
            return handle_pubcomp(session, body, body_len);
        case PacketType::SUBSCRIBE:
            return handle_subscribe(session, body, body_len);
        case PacketType::UNSUBSCRIBE:
            return handle_unsubscribe(session, body, body_len);
        case PacketType::PINGREQ:
            return handle_pingreq(session);
        case PacketType::DISCONNECT:
            return handle_disconnect(session, body, body_len);
        case PacketType::AUTH:
            return handle_auth(session, body, body_len);
        default:
            return {};
        }
    }

    ByteBuffer MqttDispatcher::handle_connect(MqttSession & session, const uint8_t * data, size_t len)
    {
        auto connect_opt = MqttCodec::decode_connect(data, len);
        if (!connect_opt.has_value()) {
            session.set_state(MqttSessionState::disconnecting);
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);
        }

        auto &connect = *connect_opt;
        bool is_v5 = (connect.protocol_level == ProtocolLevel::V5_0);

        bool version_supported = false;
        for (auto v : config_.supported_versions) {
            if (v == connect.protocol_level) {
                version_supported = true;
                break;
            }
        }

        if (!version_supported) {
            session.set_protocol_level(connect.protocol_level);
            session.set_state(MqttSessionState::disconnecting);
            MqttConnackPacket connack;
            connack.session_present = 0;
            connack.reason_code = is_v5
                                      ? static_cast<uint8_t>(ConnackCode::V5_UNSUPPORTED_PROTOCOL_VERSION)
                                      : static_cast<uint8_t>(ConnackCode::UNACCEPTABLE_PROTOCOL_VERSION);
            return MqttCodec::encode_connack(connack, connect.protocol_level);
        }

        if (connect.client_id.empty()) {
            if (!is_v5) {
                session.set_protocol_level(connect.protocol_level);
                session.set_state(MqttSessionState::disconnecting);
                MqttConnackPacket connack;
                connack.session_present = 0;
                connack.reason_code = static_cast<uint8_t>(ConnackCode::IDENTIFIER_REJECTED);
                return MqttCodec::encode_connack(connack, connect.protocol_level);
            }
            connect.client_id = "auto-" + std::to_string(session.session_id());
        }

        MqttSession *old_session = session_mgr_.find_by_client_id(connect.client_id);
        const bool clean_start = (connect.connect_flags & MQTT_CONNECT_FLAG_CLEAN_START) != 0;
        if (old_session && old_session != &session) {
            const bool old_online = old_session->state() == MqttSessionState::connected ||
                                    old_session->state() == MqttSessionState::connecting;
            if (old_online || clean_start) {
                if (handler_) {
                    handler_->on_disconnected(old_session, 0);
                }
                old_session->set_state(MqttSessionState::disconnecting);
                if (old_session->connection()) {
                    old_session->connection()->close();
                }
            }
        }

        std::string username;
        std::string password;
        if (connect.connect_flags & MQTT_CONNECT_FLAG_USERNAME) {
            if (connect.username.has_value())
                username = *connect.username;
        }
        if (connect.connect_flags & MQTT_CONNECT_FLAG_PASSWORD) {
            if (connect.password.has_value())
                password = *connect.password;
        }

        if (config_.require_authentication && username.empty()) {
            session.set_protocol_level(connect.protocol_level);
            session.set_state(MqttSessionState::disconnecting);
            MqttConnackPacket connack;
            connack.session_present = 0;
            connack.reason_code = is_v5
                                      ? static_cast<uint8_t>(ConnackCode::V5_BAD_USER_NAME_OR_PASSWORD)
                                      : static_cast<uint8_t>(ConnackCode::BAD_USER_NAME_OR_PASSWORD);
            return MqttCodec::encode_connack(connack, connect.protocol_level);
        }

        if (handler_ && !handler_->on_connect(&session, connect.client_id, username, password)) {
            session.set_protocol_level(connect.protocol_level);
            session.set_state(MqttSessionState::disconnecting);
            MqttConnackPacket connack;
            connack.session_present = 0;
            connack.reason_code = is_v5
                                      ? static_cast<uint8_t>(ConnackCode::V5_NOT_AUTHORIZED)
                                      : static_cast<uint8_t>(ConnackCode::NOT_AUTHORIZED);
            return MqttCodec::encode_connack(connack, connect.protocol_level);
        }

        bool session_present = 0;
        if (!connect.client_id.empty() && !clean_start) {
            if (MqttSession *existing = session_mgr_.find_by_client_id(connect.client_id)) {
                if (existing != &session && existing->state() == MqttSessionState::disconnected &&
                    existing->session_expiry_interval() > 0) {
                    session_present = 1;
                }
            }
        }

        if (clean_start) {
            if (MqttSession *existing = session_mgr_.find_by_client_id(connect.client_id)) {
                if (existing != &session) {
                    topic_tree_.remove_all(existing->session_id());
                }
            }
        }

        session_mgr_.bind_client_id(session, connect.client_id);
        session.set_protocol_level(connect.protocol_level);
        session.set_keep_alive(connect.keep_alive == 0 ? config_.keep_alive_default : connect.keep_alive);
        session.set_clean_start(connect.connect_flags & MQTT_CONNECT_FLAG_CLEAN_START);
        session.set_state(MqttSessionState::connected);

        if (connect.connect_flags & MQTT_CONNECT_FLAG_WILL_FLAG) {
            MqttWillMessage will;
            will.topic = connect.will_topic.value_or("");
            will.payload = connect.will_payload.value_or(std::vector<uint8_t>{});
            will.qos = static_cast<QoS>((connect.connect_flags >> MQTT_CONNECT_FLAG_WILL_QOS_SHIFT) & 0x03);
            will.retain = (connect.connect_flags & MQTT_CONNECT_FLAG_WILL_RETAIN) != 0;
            if (is_v5) {
                will.will_delay_interval = connect.will_properties.will_delay_interval;
                will.payload_format_indicator = connect.will_properties.payload_format_indicator;
                will.message_expiry_interval = connect.will_properties.message_expiry_interval;
                will.content_type = connect.will_properties.content_type;
                will.response_topic = connect.will_properties.response_topic;
                will.correlation_data = connect.will_properties.correlation_data;
                will.user_properties = connect.will_properties.user_properties;
            }
            session.set_will_message(std::move(will));
        }

        if (is_v5) {
            if (connect.properties.receive_maximum.has_value())
                session.set_client_receive_maximum(*connect.properties.receive_maximum);
            session.set_receive_maximum(std::min(
                session.client_receive_maximum(),
                config_.receive_maximum));

            if (connect.properties.topic_alias_maximum.has_value())
                session.set_topic_alias_maximum(*connect.properties.topic_alias_maximum);
            else
                session.set_topic_alias_maximum(config_.topic_alias_maximum);

            if (connect.properties.maximum_packet_size.has_value())
                session.set_maximum_packet_size(*connect.properties.maximum_packet_size);

            if (connect.properties.session_expiry_interval.has_value())
                session.set_session_expiry_interval(*connect.properties.session_expiry_interval);
        }

        MqttConnackPacket connack;
        connack.session_present = session_present;
        connack.reason_code = static_cast<uint8_t>(ConnackCode::ACCEPTED);

        if (is_v5) {
            connack.properties.receive_maximum = config_.receive_maximum;
            connack.properties.topic_alias_maximum = config_.topic_alias_maximum;
            connack.properties.maximum_qos = config_.maximum_qos;
            connack.properties.retain_available = config_.retain_available ? 1 : 0;
            connack.properties.wildcard_subscription_available = config_.wildcard_subscription_available ? 1 : 0;
            connack.properties.subscription_identifier_available = config_.subscription_identifier_available ? 1 : 0;
            connack.properties.shared_subscription_available = config_.shared_subscription_available ? 1 : 0;
            connack.properties.maximum_packet_size = config_.max_packet_size;
            connack.properties.assigned_client_identifier = connect.client_id;
            if (connect.keep_alive == 0 && config_.keep_alive_default != MQTT_KEEP_ALIVE_DEFAULT) {
                connack.properties.server_keep_alive = config_.keep_alive_default;
                session.set_keep_alive(config_.keep_alive_default);
            }
        }

        if (handler_)
            handler_->on_connected(&session);

        return MqttCodec::encode_connack(connack, session.protocol_level());
    }

    ByteBuffer MqttDispatcher::handle_publish(MqttSession & session, const uint8_t * data, size_t len, uint8_t flags)
    {
        uint8_t dup = (flags & MQTT_PUBLISH_FLAG_DUP) ? 1 : 0;
        QoS qos = static_cast<QoS>((flags >> MQTT_PUBLISH_FLAG_QOS_SHIFT) & 0x03);
        uint8_t retain = (flags & MQTT_PUBLISH_FLAG_RETAIN) ? 1 : 0;

        auto publish_opt = MqttCodec::decode_publish(data, len, flags, session.protocol_level());
        if (!publish_opt.has_value()) {
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);
        }

        auto &pkt = *publish_opt;

        bool is_v5 = (session.protocol_level() == ProtocolLevel::V5_0);
        if (is_v5 && pkt.properties.topic_alias.has_value()) {
            uint16_t alias = *pkt.properties.topic_alias;
            if (alias == 0) {
                return protocol_error_disconnect(session, DisconnectReason::TOPIC_ALIAS_INVALID);
            }
            if (session.topic_alias_maximum() > 0 && alias > session.topic_alias_maximum()) {
                return protocol_error_disconnect(session, DisconnectReason::TOPIC_ALIAS_INVALID);
            }
            if (pkt.topic.empty()) {
                auto resolved = session.resolve_topic_alias(alias);
                if (!resolved.has_value())
                    return protocol_error_disconnect(session, DisconnectReason::TOPIC_ALIAS_INVALID);
                pkt.topic = *resolved;
            } else {
                session.set_topic_alias(alias, pkt.topic);
            }
        }

        if (is_v5 && pkt.properties.topic_alias.has_value() && pkt.topic.empty()) {
            return protocol_error_disconnect(session, DisconnectReason::TOPIC_ALIAS_INVALID);
        }

        if (pkt.topic.empty() || !MqttTopicTree::validate_topic_name(pkt.topic)) {
            return protocol_error_disconnect(session, DisconnectReason::TOPIC_ALIAS_INVALID);
        }

        if (qos != QoS::AT_MOST_ONCE && !pkt.packet_id.has_value()) {
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);
        }

        if (qos != QoS::AT_MOST_ONCE && pkt.packet_id.value_or(0) == 0) {
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);
        }

        if (qos == QoS::EXACTLY_ONCE) {
            if (session.inflight_count() >= session.receive_maximum()) {
                return protocol_error_disconnect(session, DisconnectReason::RECEIVE_MAXIMUM_EXCEEDED);
            }
            if (pkt.packet_id.has_value() && session.has_inflight_packet_id(*pkt.packet_id)) {
                return MqttCodec::encode_pubrec(
                    *pkt.packet_id,
                    static_cast<uint8_t>(PubrecReason::PACKET_IDENTIFIER_IN_USE),
                    session.protocol_level(), {});
            }
        }

        if (handler_ && !handler_->on_publish(&session, pkt.topic, pkt.payload, qos, retain)) {
            if (qos == QoS::AT_LEAST_ONCE) {
                return MqttCodec::encode_puback(
                    pkt.packet_id.value_or(0),
                    static_cast<uint8_t>(PubackReason::NOT_AUTHORIZED),
                    session.protocol_level(), {});
            }
            if (qos == QoS::EXACTLY_ONCE) {
                return MqttCodec::encode_pubrec(
                    pkt.packet_id.value_or(0),
                    static_cast<uint8_t>(PubrecReason::NOT_AUTHORIZED),
                    session.protocol_level(), {});
            }
            return {};
        }

        if (retain) {
            if (pkt.payload.empty()) {
                MqttRetainedMessage dummy;
                dummy.topic = pkt.topic;
                retained_store_.store(dummy);
            } else {
                MqttRetainedMessage rm;
                rm.topic = pkt.topic;
                rm.payload = pkt.payload;
                rm.qos = qos;
                if (is_v5) {
                    rm.payload_format_indicator = pkt.properties.payload_format_indicator;
                    rm.message_expiry_interval = pkt.properties.message_expiry_interval;
                    rm.content_type = pkt.properties.content_type;
                    rm.user_properties = pkt.properties.user_properties;
                }
                retained_store_.store(rm);
            }
        }

        publish_to_subscribers(session, pkt);

        if (qos == QoS::AT_MOST_ONCE) {
            return {};
        } else if (qos == QoS::AT_LEAST_ONCE) {
            return MqttCodec::encode_puback(
                pkt.packet_id.value_or(0),
                static_cast<uint8_t>(PubackReason::SUCCESS),
                session.protocol_level(), {});
        } else {
            uint16_t pid = pkt.packet_id.value_or(0);
            session.add_inflight_packet_id(pid);
            return MqttCodec::encode_pubrec(
                pid,
                static_cast<uint8_t>(PubrecReason::SUCCESS),
                session.protocol_level(), {});
        }
    }

    ByteBuffer MqttDispatcher::handle_puback(MqttSession & session, const uint8_t * data, size_t len)
    {
        auto puback_opt = MqttCodec::decode_puback(data, len, session.protocol_level());
        if (!puback_opt.has_value())
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);

        if (handler_)
            handler_->on_message_delivered(&session, puback_opt->packet_id);

        return {};
    }

    ByteBuffer MqttDispatcher::handle_pubrec(MqttSession & session, const uint8_t * data, size_t len)
    {
        auto pubrec_opt = MqttCodec::decode_pubrec(data, len, session.protocol_level());
        if (!pubrec_opt.has_value())
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);

        if (pubrec_opt->packet_id == 0)
            return protocol_error_disconnect(session, DisconnectReason::PROTOCOL_ERROR);

        return MqttCodec::encode_pubrel(
            pubrec_opt->packet_id,
            static_cast<uint8_t>(PubrelReason::SUCCESS),
            session.protocol_level(), {});
    }

    ByteBuffer MqttDispatcher::handle_pubrel(MqttSession & session, const uint8_t * data, size_t len)
    {
        auto pubrel_opt = MqttCodec::decode_pubrel(data, len, session.protocol_level());
        if (!pubrel_opt.has_value())
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);

        uint16_t pid = pubrel_opt->packet_id;
        if (pid == 0)
            return protocol_error_disconnect(session, DisconnectReason::PROTOCOL_ERROR);

        if (!session.has_inflight_packet_id(pid)) {
            return MqttCodec::encode_pubcomp(
                pid,
                static_cast<uint8_t>(PubcompReason::PACKET_IDENTIFIER_NOT_FOUND),
                session.protocol_level(), {});
        }

        session.remove_inflight_packet_id(pid);

        return MqttCodec::encode_pubcomp(
            pid,
            static_cast<uint8_t>(PubcompReason::SUCCESS),
            session.protocol_level(), {});
    }

    ByteBuffer MqttDispatcher::handle_pubcomp(MqttSession & session, const uint8_t * data, size_t len)
    {
        auto pubcomp_opt = MqttCodec::decode_pubcomp(data, len, session.protocol_level());
        if (!pubcomp_opt.has_value())
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);

        session.remove_outgoing_packet_id(pubcomp_opt->packet_id);
        return {};
    }

    ByteBuffer MqttDispatcher::handle_subscribe(MqttSession & session, const uint8_t * data, size_t len)
    {
        auto sub_opt = MqttCodec::decode_subscribe(data, len, session.protocol_level());
        if (!sub_opt.has_value())
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);

        auto &sub_pkt = *sub_opt;
        if (sub_pkt.packet_id == 0 || sub_pkt.subscriptions.empty())
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);

        std::vector<uint8_t> reason_codes;
        reason_codes.reserve(sub_pkt.subscriptions.size());

        bool is_v5 = (session.protocol_level() == ProtocolLevel::V5_0);

        for (auto &sub : sub_pkt.subscriptions) {
            if (!MqttTopicTree::validate_topic_filter(sub.topic_filter)) {
                reason_codes.push_back(is_v5
                                           ? static_cast<uint8_t>(SubackReason::TOPIC_FILTER_INVALID)
                                           : static_cast<uint8_t>(SubackReason::UNSPECIFIED_ERROR));
                continue;
            }

            if (MqttTopicTree::is_shared_subscription(sub.topic_filter) && !config_.shared_subscription_available) {
                reason_codes.push_back(is_v5
                                           ? static_cast<uint8_t>(SubackReason::SHARED_SUBSCRIPTION_NOT_SUPPORTED)
                                           : static_cast<uint8_t>(SubackReason::UNSPECIFIED_ERROR));
                continue;
            }

            if (!config_.wildcard_subscription_available &&
                (sub.topic_filter.find('+') != std::string::npos || sub.topic_filter.find('#') != std::string::npos)) {
                reason_codes.push_back(is_v5
                                           ? static_cast<uint8_t>(SubackReason::WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED)
                                           : static_cast<uint8_t>(SubackReason::UNSPECIFIED_ERROR));
                continue;
            }

            if (is_v5 && sub_pkt.properties.subscription_identifier.has_value() &&
                !config_.subscription_identifier_available) {
                reason_codes.push_back(static_cast<uint8_t>(SubackReason::SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED));
                continue;
            }

            if (handler_ && !handler_->on_subscribe(&session, sub.topic_filter, sub.maximum_qos)) {
                reason_codes.push_back(is_v5
                                           ? static_cast<uint8_t>(SubackReason::NOT_AUTHORIZED)
                                           : static_cast<uint8_t>(SubackReason::UNSPECIFIED_ERROR));
                continue;
            }

            QoS granted_qos = sub.maximum_qos;
            if (static_cast<uint8_t>(granted_qos) > config_.maximum_qos)
                granted_qos = static_cast<QoS>(config_.maximum_qos);

            MqttSubscription tree_sub;
            tree_sub.session_id = session.session_id();
            tree_sub.qos = granted_qos;
            tree_sub.no_local = sub.no_local;
            tree_sub.retain_as_published = sub.retain_as_published;
            tree_sub.retain_handling = sub.retain_handling;
            if (is_v5 && sub_pkt.properties.subscription_identifier.has_value())
                tree_sub.subscription_identifier = sub_pkt.properties.subscription_identifier;
            if (auto sg = extract_shared_group(sub.topic_filter); sg.has_value()) {
                tree_sub.shared_group = *sg;
                tree_sub.shared_filter = sub.topic_filter;
            }

            topic_tree_.subscribe(sub.topic_filter, tree_sub);
            session.add_subscription(sub.topic_filter, granted_qos);

            auto retained_msgs = retained_store_.match(sub.topic_filter);
            for (auto &rm : retained_msgs) {
                if (sub.retain_handling == 2)
                    continue;

                MqttPublishPacket pub;
                pub.retain = 1;
                pub.topic = rm.topic;
                pub.payload = rm.payload;

                QoS ret_qos = granted_qos;
                if (static_cast<uint8_t>(ret_qos) > static_cast<uint8_t>(rm.qos))
                    ret_qos = rm.qos;
                pub.qos = ret_qos;

                if (ret_qos != QoS::AT_MOST_ONCE)
                    pub.packet_id = session.next_packet_id();

                if (is_v5) {
                    pub.properties.payload_format_indicator = rm.payload_format_indicator;
                    pub.properties.message_expiry_interval = rm.message_expiry_interval;
                    pub.properties.content_type = rm.content_type;
                    pub.properties.user_properties = rm.user_properties;
                }

                auto conn = session.connection();
                if (conn) {
                    auto pub_buf = MqttCodec::encode_publish(pub, session.protocol_level());
                    conn->write_and_flush(pub_buf);
                }
            }

            uint8_t rc = static_cast<uint8_t>(granted_qos);
            reason_codes.push_back(rc);
        }

        return MqttCodec::encode_suback(sub_pkt.packet_id, reason_codes, session.protocol_level(), {});
    }

    ByteBuffer MqttDispatcher::handle_unsubscribe(MqttSession & session, const uint8_t * data, size_t len)
    {
        auto unsub_opt = MqttCodec::decode_unsubscribe(data, len, session.protocol_level());
        if (!unsub_opt.has_value())
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);

        auto &unsub_pkt = *unsub_opt;
        if (unsub_pkt.packet_id == 0 || unsub_pkt.topic_filters.empty())
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);

        std::vector<uint8_t> reason_codes;
        reason_codes.reserve(unsub_pkt.topic_filters.size());

        bool is_v5 = (session.protocol_level() == ProtocolLevel::V5_0);

        for (auto &filter : unsub_pkt.topic_filters) {
            topic_tree_.unsubscribe(filter, session.session_id());
            session.remove_subscription(filter);

            if (handler_)
                handler_->on_unsubscribe(&session, filter);

            reason_codes.push_back(is_v5
                                       ? static_cast<uint8_t>(UnsubackReason::SUCCESS)
                                       : static_cast<uint8_t>(UnsubackReason::SUCCESS));
        }

        return MqttCodec::encode_unsuback(unsub_pkt.packet_id, reason_codes, session.protocol_level(), {});
    }

    ByteBuffer MqttDispatcher::handle_pingreq(MqttSession & session)
    {
        session.update_last_activity();
        return MqttCodec::encode_pingresp();
    }

    ByteBuffer MqttDispatcher::handle_disconnect(MqttSession & session, const uint8_t * data, size_t len)
    {
        const bool was_connected = (session.state() == MqttSessionState::connected);
        session.set_state(MqttSessionState::disconnected);

        bool clear_will = true;
        if (session.protocol_level() == ProtocolLevel::V5_0 && len > 0) {
            auto disc_opt = MqttCodec::decode_disconnect(data, len);
            if (disc_opt.has_value() &&
                disc_opt->reason_code == static_cast<uint8_t>(DisconnectReason::DISCONNECT_WITH_WILL_MESSAGE)) {
                clear_will = false;
            }
        }

        if (clear_will)
            session.clear_will_message();

        if (was_connected && session.session_expiry_interval() == 0) {
            topic_tree_.remove_all(session.session_id());
        }

        return {};
    }

    ByteBuffer MqttDispatcher::handle_auth(MqttSession & session, const uint8_t * data, size_t len)
    {
        if (session.protocol_level() != ProtocolLevel::V5_0) {
            return {};
        }

        auto auth_opt = MqttCodec::decode_auth(data, len);
        if (!auth_opt.has_value())
            return protocol_error_disconnect(session, DisconnectReason::MALFORMED_PACKET);

        std::string method;
        std::vector<uint8_t> auth_data;
        if (auth_opt->properties.authentication_method.has_value())
            method = *auth_opt->properties.authentication_method;
        if (auth_opt->properties.authentication_data.has_value())
            auth_data = *auth_opt->properties.authentication_data;

        if (method.empty()) {
            return protocol_error_disconnect(session, DisconnectReason::BAD_AUTHENTICATION_METHOD);
        }

        if (handler_ && handler_->on_auth(&session, method, auth_data)) {
            MqttAuthPacket response;
            response.reason_code = static_cast<uint8_t>(AuthReason::SUCCESS);
            return MqttCodec::encode_auth(response.reason_code, response.properties);
        }

        return MqttCodec::encode_disconnect(
            static_cast<uint8_t>(DisconnectReason::BAD_AUTHENTICATION_METHOD),
            session.protocol_level(), {});
    }

    void MqttDispatcher::publish_to_subscribers(MqttSession & source, const MqttPublishPacket & pkt)
    {
        auto matches = topic_tree_.match(pkt.topic);
        if (matches.empty())
            return;

        const auto selected_matches = apply_shared_subscription_policy(matches);

        for (auto &sub : selected_matches) {
            if (sub.no_local && sub.session_id == source.session_id())
                continue;

            MqttSession *sub_session = nullptr;
            auto all = session_mgr_.all_sessions();
            for (auto *s : all) {
                if (s->session_id() == sub.session_id) {
                    sub_session = s;
                    break;
                }
            }
            if (!sub_session || sub_session->state() != MqttSessionState::connected)
                continue;
            auto sub_conn = sub_session->connection();
            if (!sub_conn)
                continue;

            MqttPublishPacket pub;
            pub.topic = pkt.topic;
            pub.payload = pkt.payload;
            pub.dup = 0;
            pub.retain = (sub.retain_as_published == 1) ? pkt.retain : 0;

            QoS pub_qos = pkt.qos;
            if (static_cast<uint8_t>(pub_qos) > static_cast<uint8_t>(sub.qos))
                pub_qos = sub.qos;
            pub.qos = pub_qos;

            if (pub_qos != QoS::AT_MOST_ONCE)
                pub.packet_id = sub_session->next_packet_id();

            bool is_v5 = (sub_session->protocol_level() == ProtocolLevel::V5_0);
            if (is_v5) {
                pub.properties = pkt.properties;
                pub.properties.subscription_identifier.reset();
                if (sub.subscription_identifier.has_value())
                    pub.properties.subscription_identifier = sub.subscription_identifier;
            }

            auto pub_buf = MqttCodec::encode_publish(pub, sub_session->protocol_level());
            sub_conn->write_and_flush(pub_buf);
        }
    }

    void MqttDispatcher::on_session_closed(MqttSession & session)
    {
        bool should_send_will = true;
        if (session.protocol_level() == ProtocolLevel::V5_0) {
            auto *will = session.will_message();
            if (will && will->will_delay_interval.has_value() && *will->will_delay_interval > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - session.last_activity());
                if (elapsed.count() < static_cast<int64_t>(*will->will_delay_interval)) {
                    should_send_will = false;
                }
            }
        }

        if (should_send_will) {
            send_will_message(session);
        }

        if (session.session_expiry_interval() == 0) {
            topic_tree_.remove_all(session.session_id());
        }

        if (handler_)
            handler_->on_disconnected(&session, 0);

        session.set_state(MqttSessionState::disconnected);
    }

    void MqttDispatcher::send_will_message(MqttSession & session)
    {
        auto *will = session.will_message();
        if (!will)
            return;

        MqttPublishPacket pub;
        pub.topic = will->topic;
        pub.payload = will->payload;
        pub.qos = will->qos;
        pub.retain = will->retain ? 1 : 0;
        pub.dup = 0;

        bool is_v5 = (session.protocol_level() == ProtocolLevel::V5_0);
        if (is_v5) {
            pub.properties.payload_format_indicator = will->payload_format_indicator;
            pub.properties.message_expiry_interval = will->message_expiry_interval;
            pub.properties.content_type = will->content_type;
            pub.properties.response_topic = will->response_topic;
            pub.properties.correlation_data = will->correlation_data;
            pub.properties.user_properties = will->user_properties;
        }

        publish_to_subscribers(session, pub);

        if (will->retain) {
            MqttRetainedMessage rm;
            rm.topic = will->topic;
            rm.payload = will->payload;
            rm.qos = will->qos;
            if (is_v5) {
                rm.payload_format_indicator = will->payload_format_indicator;
                rm.message_expiry_interval = will->message_expiry_interval;
                rm.content_type = will->content_type;
                rm.user_properties = will->user_properties;
            }
            retained_store_.store(rm);
        }

        session.clear_will_message();
    }

    ByteBuffer MqttDispatcher::build_publish_for_session(MqttSession & session,
                                                         const std::string & topic,
                                                         const std::vector<uint8_t> & payload,
                                                         QoS qos, bool retain,
                                                         const MqttProperties & props)
    {
        MqttPublishPacket pub;
        pub.topic = topic;
        pub.payload = payload;
        pub.qos = qos;
        pub.retain = retain ? 1 : 0;
        pub.dup = 0;

        if (qos != QoS::AT_MOST_ONCE)
            pub.packet_id = session.next_packet_id();

        pub.properties = props;

        return MqttCodec::encode_publish(pub, session.protocol_level());
    }
}
