#ifndef __RELEASE_MQTT_BUSINESS_EXAMPLE_H__
#define __RELEASE_MQTT_BUSINESS_EXAMPLE_H__

#include "mqtt_business_handler.h"
#include "mqtt_business_options.h"

#include <functional>

namespace yuan::release::mqtt
{
    class ExampleMqttBusinessHandler final : public IMqttBusinessHandler
    {
    public:
        using PublishRouteHandler = std::function<bool(const std::string &, const std::vector<uint8_t> &)>;
        using SubscribeRouteHandler = std::function<bool(const std::string &, yuan::net::mqtt::QoS)>;

        struct PublishRoute
        {
            std::string topic_prefix;
            PublishRouteHandler handler;
        };

        struct SubscribeRoute
        {
            std::string topic_prefix;
            SubscribeRouteHandler handler;
        };

        explicit ExampleMqttBusinessHandler(MqttBusinessRouteOptions options = {});

        void register_publish_route(std::string topic_prefix, PublishRouteHandler handler);
        void clear_publish_routes();
        void register_subscribe_route(std::string topic_prefix, SubscribeRouteHandler handler);
        void clear_subscribe_routes();

        bool on_connect(yuan::net::mqtt::MqttSession *session,
                        const std::string &client_id,
                        const std::string &username,
                        const std::string &password) override;

        bool on_publish(yuan::net::mqtt::MqttSession *session,
                        const std::string &topic,
                        const std::vector<uint8_t> &payload,
                        yuan::net::mqtt::QoS qos,
                        bool retain) override;

        bool on_subscribe(yuan::net::mqtt::MqttSession *session,
                          const std::string &topic_filter,
                          yuan::net::mqtt::QoS qos) override;

    private:
        bool dispatch_publish_topic(const std::string &topic, const std::vector<uint8_t> &payload);
        bool dispatch_subscribe_topic(const std::string &topic_filter, yuan::net::mqtt::QoS qos);
        bool handle_default_publish(const std::string &topic, const std::vector<uint8_t> &payload);
        bool handle_default_subscribe(const std::string &topic_filter, yuan::net::mqtt::QoS qos);

        std::vector<PublishRoute> publish_routes_;
        std::vector<SubscribeRoute> subscribe_routes_;
        MqttBusinessRouteOptions options_;
    };
}

#endif
