#ifndef __NET_MQTT_MQTT_TOPIC_TREE_H__
#define __NET_MQTT_MQTT_TOPIC_TREE_H__

#include "mqtt_protocol.h"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::mqtt
{
    struct MqttSubscription
    {
        uint64_t session_id;
        QoS qos = QoS::AT_MOST_ONCE;
        uint8_t no_local = 0;
        uint8_t retain_as_published = 0;
        uint8_t retain_handling = 0;
        std::optional<uint32_t> subscription_identifier;
    };

    class MqttTopicTree
    {
    public:
        std::optional<QoS> subscribe(const std::string &topic_filter, const MqttSubscription &sub);
        bool unsubscribe(const std::string &topic_filter, uint64_t session_id);
        std::vector<MqttSubscription> match(const std::string &topic) const;
        std::vector<std::string> subscriptions(uint64_t session_id) const;
        void remove_all(uint64_t session_id);

        static bool validate_topic_filter(const std::string &filter);
        static bool validate_topic_name(const std::string &topic);
        static bool is_shared_subscription(const std::string &filter);
        static std::string shared_group(const std::string &filter);
        static std::string shared_topic_filter(const std::string &filter);

    private:
        struct Node
        {
            std::map<std::string, Node> children;
            std::unique_ptr<Node> single_level_wildcard;
            std::unique_ptr<Node> multi_level_wildcard;
            std::vector<MqttSubscription> subscriptions;
        };

        Node root_;

        void match_recursive(const Node &node, const std::vector<std::string> &levels,
                             size_t level_index, std::vector<MqttSubscription> &result) const;
        Node *find_or_create_node(const std::string &topic_filter);
    };
}

#endif
