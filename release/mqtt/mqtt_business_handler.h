#ifndef __RELEASE_MQTT_BUSINESS_HANDLER_H__
#define __RELEASE_MQTT_BUSINESS_HANDLER_H__

#include "mqtt.h"
#include "mqtt_service_enhanced_handler.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::release::mqtt
{
    class IMqttBusinessHandler
    {
    public:
        virtual ~IMqttBusinessHandler() = default;

        virtual bool on_connect(yuan::net::mqtt::MqttSession *session,
                                const std::string &client_id,
                                const std::string &username,
                                const std::string &password)
        {
            return true;
        }

        virtual bool on_auth(yuan::net::mqtt::MqttSession *session,
                             const std::string &method,
                             const std::vector<uint8_t> &data)
        {
            return true;
        }

        virtual void on_connected(yuan::net::mqtt::MqttSession *session)
        {
        }

        virtual void on_disconnected(yuan::net::mqtt::MqttSession *session, uint8_t reason_code)
        {
        }

        virtual bool on_publish(yuan::net::mqtt::MqttSession *session,
                                const std::string &topic,
                                const std::vector<uint8_t> &payload,
                                yuan::net::mqtt::QoS qos,
                                bool retain)
        {
            return true;
        }

        virtual bool on_subscribe(yuan::net::mqtt::MqttSession *session,
                                  const std::string &topic_filter,
                                  yuan::net::mqtt::QoS qos)
        {
            return true;
        }

        virtual void on_unsubscribe(yuan::net::mqtt::MqttSession *session,
                                    const std::string &topic_filter)
        {
        }

        virtual void on_message_delivered(yuan::net::mqtt::MqttSession *session, uint16_t packet_id)
        {
        }
    };

    class DefaultMqttBusinessHandler final : public IMqttBusinessHandler
    {
    };

    class CompositeMqttHandler final : public yuan::net::mqtt::MqttHandler
    {
    public:
        CompositeMqttHandler(yuan::server::MqttEnhancedHandler *enhanced,
                             IMqttBusinessHandler *business);

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

        void on_unsubscribe(yuan::net::mqtt::MqttSession *session,
                            const std::string &topic_filter) override;

        void on_message_delivered(yuan::net::mqtt::MqttSession *session, uint16_t packet_id) override;

    private:
        yuan::server::MqttEnhancedHandler *enhanced_ = nullptr;
        IMqttBusinessHandler *business_ = nullptr;
    };
}

#endif
