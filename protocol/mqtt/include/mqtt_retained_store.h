#ifndef __NET_MQTT_MQTT_RETAINED_STORE_H__
#define __NET_MQTT_MQTT_RETAINED_STORE_H__

#include "mqtt_protocol.h"
#include "mqtt_properties.h"
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::mqtt
{
    struct MqttRetainedMessage
    {
        std::string topic;
        std::vector<uint8_t> payload;
        QoS qos = QoS::AT_MOST_ONCE;
        std::chrono::steady_clock::time_point stored_time;
        std::optional<uint32_t> message_expiry_interval;
        std::optional<uint8_t> payload_format_indicator;
        std::optional<std::string> content_type;
        std::vector<UserProperty> user_properties;
    };

    class MqttRetainedStore
    {
    public:
        void store(const MqttRetainedMessage &msg);
        std::vector<MqttRetainedMessage> match(const std::string &topic_filter) const;
        void cleanup_expired();
        size_t size() const;
        bool save_to_file(const std::string &path) const;
        bool load_from_file(const std::string &path);

    private:
        std::map<std::string, MqttRetainedMessage> messages_;
    };
}

#endif
