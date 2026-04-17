#ifndef __NET_MQTT_MQTT_SERVER_H__
#define __NET_MQTT_MQTT_SERVER_H__

#include "mqtt_config.h"
#include "mqtt_handler.h"
#include "mqtt_session.h"
#include "mqtt_topic_tree.h"
#include "mqtt_retained_store.h"
#include "mqtt_dispatcher.h"
#include "mqtt_protocol.h"
#include "coroutine/task.h"
#include "net/async/async_listener_host.h"
#include "net/async/async_connection_context.h"
#include "net/runtime/network_runtime.h"
#include "buffer/byte_buffer.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace yuan::net::mqtt
{
    class MqttServer
    {
    public:
        MqttServer();
        explicit MqttServer(const MqttServerConfig &config);
        ~MqttServer();

        MqttServer(const MqttServer &) = delete;
        MqttServer &operator=(const MqttServer &) = delete;

        bool init(int port);
        bool init(int port, NetworkRuntime &runtime);
        void serve();
        void stop();

        NetworkRuntime *runtime() const noexcept
        {
            return listener_.runtime();
        }
        void set_handler(MqttHandler *handler);
        const MqttServerConfig &config() const
        {
            return config_;
        }

        void publish(const std::string &topic, const std::vector<uint8_t> &payload,
                     QoS qos = QoS::AT_MOST_ONCE, bool retain = false);
        size_t connected_clients() const;

    private:
        coroutine::Task<void> handle_connection(AsyncConnectionContext ctx);

        AsyncListenerHost listener_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
        MqttServerConfig config_;

        MqttSessionManager session_mgr_;
        MqttTopicTree topic_tree_;
        MqttRetainedStore retained_store_;
        MqttDispatcher dispatcher_;
        MqttHandler *handler_ = nullptr;
    };
}

#endif
