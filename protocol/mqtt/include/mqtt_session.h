#ifndef __NET_MQTT_MQTT_SESSION_H__
#define __NET_MQTT_MQTT_SESSION_H__

#include "mqtt_protocol.h"
#include "mqtt_properties.h"
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace yuan::net
{
    class TcpConnection;
}

namespace yuan::net::mqtt
{
    enum class MqttSessionState {
        disconnected,
        connecting,
        connected,
        disconnecting
    };

    struct MqttWillMessage
    {
        std::string topic;
        std::vector<uint8_t> payload;
        QoS qos = QoS::AT_MOST_ONCE;
        bool retain = false;
        std::optional<uint32_t> will_delay_interval;
        std::optional<uint8_t> payload_format_indicator;
        std::optional<uint32_t> message_expiry_interval;
        std::optional<std::string> content_type;
        std::optional<std::string> response_topic;
        std::optional<std::vector<uint8_t> > correlation_data;
        std::vector<UserProperty> user_properties;
    };

    class MqttSession
    {
    public:
        explicit MqttSession(TcpConnection *conn);

        uint64_t session_id() const
        {
            return session_id_;
        }
        const std::string &client_id() const
        {
            return client_id_;
        }
        void set_client_id(const std::string &id)
        {
            client_id_ = id;
        }
        ProtocolLevel protocol_level() const
        {
            return protocol_level_;
        }
        void set_protocol_level(ProtocolLevel level)
        {
            protocol_level_ = level;
        }
        MqttSessionState state() const
        {
            return state_;
        }
        void set_state(MqttSessionState s)
        {
            state_ = s;
        }

        uint16_t keep_alive() const
        {
            return keep_alive_;
        }
        void set_keep_alive(uint16_t s)
        {
            keep_alive_ = s;
        }
        void update_last_activity()
        {
            last_activity_ = std::chrono::steady_clock::now();
        }
        std::chrono::steady_clock::time_point last_activity() const
        {
            return last_activity_;
        }

        const std::map<std::string, QoS> &subscriptions() const
        {
            return subscriptions_;
        }
        void add_subscription(const std::string &filter, QoS qos)
        {
            subscriptions_[filter] = qos;
        }
        void remove_subscription(const std::string &filter)
        {
            subscriptions_.erase(filter);
        }
        QoS subscription_qos(const std::string &filter) const;

        uint16_t next_packet_id();
        bool add_inflight_packet_id(uint16_t pid);
        bool has_inflight_packet_id(uint16_t pid) const;
        void remove_inflight_packet_id(uint16_t pid);
        void add_outgoing_packet_id(uint16_t pid);
        bool has_outgoing_packet_id(uint16_t pid) const;
        void remove_outgoing_packet_id(uint16_t pid);

        const MqttWillMessage *will_message() const;
        void set_will_message(MqttWillMessage will);
        void clear_will_message();

        bool clean_start() const
        {
            return clean_start_;
        }
        void set_clean_start(bool c)
        {
            clean_start_ = c;
        }
        uint32_t session_expiry_interval() const
        {
            return session_expiry_interval_;
        }
        void set_session_expiry_interval(uint32_t i)
        {
            session_expiry_interval_ = i;
        }

        uint16_t receive_maximum() const
        {
            return receive_maximum_;
        }
        void set_receive_maximum(uint16_t m)
        {
            receive_maximum_ = m;
        }
        uint16_t client_receive_maximum() const
        {
            return client_receive_maximum_;
        }
        void set_client_receive_maximum(uint16_t m)
        {
            client_receive_maximum_ = m;
        }
        uint32_t maximum_packet_size() const
        {
            return maximum_packet_size_;
        }
        void set_maximum_packet_size(uint32_t s)
        {
            maximum_packet_size_ = s;
        }
        uint16_t topic_alias_maximum() const
        {
            return topic_alias_maximum_;
        }
        void set_topic_alias_maximum(uint16_t m)
        {
            topic_alias_maximum_ = m;
        }

        void set_topic_alias(uint16_t alias, const std::string &topic);
        std::optional<std::string> resolve_topic_alias(uint16_t alias) const;

        TcpConnection *connection() const
        {
            return conn_;
        }

    private:
        uint64_t session_id_;
        std::string client_id_;
        ProtocolLevel protocol_level_ = ProtocolLevel::V3_1_1;
        MqttSessionState state_ = MqttSessionState::disconnected;
        uint16_t keep_alive_ = MQTT_KEEP_ALIVE_DEFAULT;
        std::chrono::steady_clock::time_point last_activity_;

        std::map<std::string, QoS> subscriptions_;

        uint16_t next_packet_id_ = 1;
        std::set<uint16_t> inflight_packet_ids_;
        std::set<uint16_t> outgoing_packet_ids_;

        std::optional<MqttWillMessage> will_message_;

        bool clean_start_ = true;
        uint32_t session_expiry_interval_ = 0;
        uint16_t receive_maximum_ = 65535;
        uint16_t client_receive_maximum_ = 65535;
        uint32_t maximum_packet_size_ = 0;
        uint16_t topic_alias_maximum_ = 0;
        std::map<uint16_t, std::string> topic_aliases_;

        TcpConnection *conn_;
    };

    class MqttSessionManager
    {
    public:
        MqttSession &create_session(TcpConnection *conn);
        void remove_session(uint64_t sid);
        MqttSession *find_by_client_id(const std::string &client_id);
        MqttSession *find_by_connection(TcpConnection *conn);
        std::vector<MqttSession *> all_sessions();
        void cleanup_expired();

    private:
        std::map<uint64_t, std::unique_ptr<MqttSession> > sessions_;
        std::map<std::string, uint64_t> client_id_index_;
        uint64_t next_session_id_ = 1;
    };
}

#endif
