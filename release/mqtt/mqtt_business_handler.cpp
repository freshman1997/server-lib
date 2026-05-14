#include "mqtt_business_handler.h"

namespace yuan::release::mqtt
{
    CompositeMqttHandler::CompositeMqttHandler(yuan::server::MqttEnhancedHandler *enhanced,
                                               IMqttBusinessHandler *business)
        : enhanced_(enhanced),
          business_(business)
    {
    }

    bool CompositeMqttHandler::on_connect(yuan::net::mqtt::MqttSession *session,
                                          const std::string &client_id,
                                          const std::string &username,
                                          const std::string &password)
    {
        if (enhanced_ && !enhanced_->on_connect(session, client_id, username, password)) {
            return false;
        }
        if (business_ && !business_->on_connect(session, client_id, username, password)) {
            return false;
        }
        return true;
    }

    bool CompositeMqttHandler::on_auth(yuan::net::mqtt::MqttSession *session,
                                       const std::string &method,
                                       const std::vector<uint8_t> &data)
    {
        if (enhanced_ && !enhanced_->on_auth(session, method, data)) {
            return false;
        }
        if (business_ && !business_->on_auth(session, method, data)) {
            return false;
        }
        return true;
    }

    void CompositeMqttHandler::on_connected(yuan::net::mqtt::MqttSession *session)
    {
        if (enhanced_) {
            enhanced_->on_connected(session);
        }
        if (business_) {
            business_->on_connected(session);
        }
    }

    void CompositeMqttHandler::on_disconnected(yuan::net::mqtt::MqttSession *session, uint8_t reason_code)
    {
        if (enhanced_) {
            enhanced_->on_disconnected(session, reason_code);
        }
        if (business_) {
            business_->on_disconnected(session, reason_code);
        }
    }

    bool CompositeMqttHandler::on_publish(yuan::net::mqtt::MqttSession *session,
                                          const std::string &topic,
                                          const std::vector<uint8_t> &payload,
                                          yuan::net::mqtt::QoS qos,
                                          bool retain)
    {
        if (enhanced_ && !enhanced_->on_publish(session, topic, payload, qos, retain)) {
            return false;
        }
        if (business_ && !business_->on_publish(session, topic, payload, qos, retain)) {
            return false;
        }
        return true;
    }

    bool CompositeMqttHandler::on_subscribe(yuan::net::mqtt::MqttSession *session,
                                            const std::string &topic_filter,
                                            yuan::net::mqtt::QoS qos)
    {
        if (enhanced_ && !enhanced_->on_subscribe(session, topic_filter, qos)) {
            return false;
        }
        if (business_ && !business_->on_subscribe(session, topic_filter, qos)) {
            return false;
        }
        return true;
    }

    void CompositeMqttHandler::on_unsubscribe(yuan::net::mqtt::MqttSession *session,
                                              const std::string &topic_filter)
    {
        if (enhanced_) {
            enhanced_->on_unsubscribe(session, topic_filter);
        }
        if (business_) {
            business_->on_unsubscribe(session, topic_filter);
        }
    }

    void CompositeMqttHandler::on_message_delivered(yuan::net::mqtt::MqttSession *session, uint16_t packet_id)
    {
        if (enhanced_) {
            enhanced_->on_message_delivered(session, packet_id);
        }
        if (business_) {
            business_->on_message_delivered(session, packet_id);
        }
    }
}
