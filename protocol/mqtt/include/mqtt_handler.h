#ifndef __NET_MQTT_MQTT_HANDLER_H__
#define __NET_MQTT_MQTT_HANDLER_H__

#include "mqtt_protocol.h"
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::mqtt
{
    class MqttSession;

    class MqttHandler
    {
    public:
        virtual ~MqttHandler() = default;

        virtual bool on_connect(MqttSession *session, const std::string &client_id,
                                const std::string &username, const std::string &password)
        {
            return true;
        }

        virtual bool on_auth(MqttSession *session, const std::string &method,
                             const std::vector<uint8_t> &data)
        {
            return false;
        }

        virtual void on_connected(MqttSession *session)
        {
        }

        virtual void on_disconnected(MqttSession *session, uint8_t reason_code)
        {
        }

        virtual bool on_publish(MqttSession *session, const std::string &topic,
                                const std::vector<uint8_t> &payload, QoS qos, bool retain)
        {
            return true;
        }

        virtual bool on_subscribe(MqttSession *session, const std::string &topic_filter, QoS qos)
        {
            return true;
        }

        virtual void on_unsubscribe(MqttSession *session, const std::string &topic_filter)
        {
        }

        virtual void on_message_delivered(MqttSession *session, uint16_t packet_id)
        {
        }
    };
}

#endif
