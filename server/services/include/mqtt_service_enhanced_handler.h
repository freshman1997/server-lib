#ifndef __SERVER_MQTT_SERVICE_ENHANCED_HANDLER_H__
#define __SERVER_MQTT_SERVICE_ENHANCED_HANDLER_H__

#include "mqtt.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::server
{
    class MqttEnhancedHandler final : public yuan::net::mqtt::MqttHandler
    {
    public:
        struct AclRule
        {
            std::string topic_filter;
            bool allow = true;
        };

        struct Metrics
        {
            std::atomic<uint64_t> connect_attempts{ 0 };
            std::atomic<uint64_t> connect_accepted{ 0 };
            std::atomic<uint64_t> connect_rejected{ 0 };
            std::atomic<uint64_t> auth_attempts{ 0 };
            std::atomic<uint64_t> auth_accepted{ 0 };
            std::atomic<uint64_t> auth_rejected{ 0 };
            std::atomic<uint64_t> publish_allowed{ 0 };
            std::atomic<uint64_t> publish_denied{ 0 };
            std::atomic<uint64_t> subscribe_allowed{ 0 };
            std::atomic<uint64_t> subscribe_denied{ 0 };
            std::atomic<uint64_t> connected_sessions{ 0 };
        };

        MqttEnhancedHandler() = default;

        void set_allow_anonymous(bool allow);
        bool allow_anonymous() const;

        void set_default_publish_allow(bool allow);
        void set_default_subscribe_allow(bool allow);

        void upsert_user(const std::string &username, const std::string &password);
        void remove_user(const std::string &username);
        void clear_users();

        void add_publish_acl(const std::string &topic_filter, bool allow);
        void add_subscribe_acl(const std::string &topic_filter, bool allow);
        void clear_publish_acl();
        void clear_subscribe_acl();
        bool save_policy_file(const std::string &path) const;
        bool load_policy_file(const std::string &path);

        const Metrics &metrics() const;

        bool on_connect(yuan::net::mqtt::MqttSession *session,
                        const std::string &client_id,
                        const std::string &username,
                        const std::string &password) override;

        bool on_auth(yuan::net::mqtt::MqttSession *session,
                     const std::string &method,
                     const std::vector<uint8_t> &data) override;

        void on_connected(yuan::net::mqtt::MqttSession *session) override;
        void on_disconnected(yuan::net::mqtt::MqttSession *session, uint8_t reason_code) override;

        bool on_publish(yuan::net::mqtt::MqttSession *session,
                        const std::string &topic,
                        const std::vector<uint8_t> &payload,
                        yuan::net::mqtt::QoS qos,
                        bool retain) override;

        bool on_subscribe(yuan::net::mqtt::MqttSession *session,
                          const std::string &topic_filter,
                          yuan::net::mqtt::QoS qos) override;

    private:
        static bool topic_matches_filter(const std::string &topic, const std::string &filter);
        bool eval_acl(const std::vector<AclRule> &rules, const std::string &topic, bool default_allow) const;

        mutable std::mutex mutex_;
        bool allow_anonymous_ = true;
        bool default_publish_allow_ = true;
        bool default_subscribe_allow_ = true;
        std::unordered_map<std::string, std::string> users_;
        std::unordered_map<uint64_t, std::string> session_username_;
        std::vector<AclRule> publish_acl_;
        std::vector<AclRule> subscribe_acl_;
        Metrics metrics_;
    };
}

#endif
