#ifndef __NET_MQTT_MQTT_DISPATCHER_H__
#define __NET_MQTT_MQTT_DISPATCHER_H__

#include "mqtt_protocol.h"
#include "mqtt_config.h"
#include "mqtt_handler.h"
#include "mqtt_session.h"
#include "mqtt_topic_tree.h"
#include "mqtt_retained_store.h"
#include "mqtt_codec.h"
#include "mqtt_packet.h"
#include "mqtt_properties.h"
#include "buffer/byte_buffer.h"
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::mqtt
{
    class MqttDispatcher
    {
    public:
        MqttDispatcher(const MqttServerConfig &config,
                       MqttSessionManager &session_mgr,
                       MqttTopicTree &topic_tree,
                       MqttRetainedStore &retained_store,
                       MqttHandler *handler = nullptr);

        ByteBuffer dispatch(MqttSession &session, const uint8_t *data, size_t len);
        void on_session_closed(MqttSession &session);

        ByteBuffer build_publish_for_session(MqttSession &session,
                                             const std::string &topic,
                                             const std::vector<uint8_t> &payload,
                                             QoS qos, bool retain,
                                             const MqttProperties &props = {});

        void set_handler(MqttHandler *handler)
        {
            handler_ = handler;
        }

    private:
        ByteBuffer handle_connect(MqttSession &session, const uint8_t *data, size_t len);
        ByteBuffer handle_publish(MqttSession &session, const uint8_t *data, size_t len, uint8_t flags);
        ByteBuffer handle_puback(MqttSession &session, const uint8_t *data, size_t len);
        ByteBuffer handle_pubrec(MqttSession &session, const uint8_t *data, size_t len);
        ByteBuffer handle_pubrel(MqttSession &session, const uint8_t *data, size_t len);
        ByteBuffer handle_pubcomp(MqttSession &session, const uint8_t *data, size_t len);
        ByteBuffer handle_subscribe(MqttSession &session, const uint8_t *data, size_t len);
        ByteBuffer handle_unsubscribe(MqttSession &session, const uint8_t *data, size_t len);
        ByteBuffer handle_pingreq(MqttSession &session);
        ByteBuffer handle_disconnect(MqttSession &session, const uint8_t *data, size_t len);
        ByteBuffer handle_auth(MqttSession &session, const uint8_t *data, size_t len);

        void publish_to_subscribers(MqttSession &source, const MqttPublishPacket &pkt);
        void send_will_message(MqttSession &session);

        const MqttServerConfig &config_;
        MqttSessionManager &session_mgr_;
        MqttTopicTree &topic_tree_;
        MqttRetainedStore &retained_store_;
        MqttHandler *handler_;
    };
}

#endif
