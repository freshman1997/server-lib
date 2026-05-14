#include "mqtt_business_example.h"

#include "mqtt_business_routes.h"

#include <iostream>
#include <utility>
#include <string>

namespace
{
    bool starts_with(const std::string &value, const std::string &prefix)
    {
        return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
    }
}

namespace yuan::release::mqtt
{
    ExampleMqttBusinessHandler::ExampleMqttBusinessHandler(MqttBusinessRouteOptions options)
        : options_(options)
    {
        clear_publish_routes();
        clear_subscribe_routes();
        register_example_publish_routes(*this, options_);
        register_example_subscribe_routes(*this, options_);
    }

    void ExampleMqttBusinessHandler::register_publish_route(std::string topic_prefix,
                                                            PublishRouteHandler handler)
    {
        publish_routes_.push_back(PublishRoute{ std::move(topic_prefix), std::move(handler) });
    }

    void ExampleMqttBusinessHandler::clear_publish_routes()
    {
        publish_routes_.clear();
    }

    void ExampleMqttBusinessHandler::register_subscribe_route(std::string topic_prefix,
                                                              SubscribeRouteHandler handler)
    {
        subscribe_routes_.push_back(SubscribeRoute{ std::move(topic_prefix), std::move(handler) });
    }

    void ExampleMqttBusinessHandler::clear_subscribe_routes()
    {
        subscribe_routes_.clear();
    }

    bool ExampleMqttBusinessHandler::on_connect(yuan::net::mqtt::MqttSession *,
                                                const std::string &client_id,
                                                const std::string &username,
                                                const std::string &)
    {
        std::cout << "[biz] client connect id=" << client_id << " user=" << username << '\n';
        return true;
    }

    bool ExampleMqttBusinessHandler::on_publish(yuan::net::mqtt::MqttSession *,
                                                const std::string &topic,
                                                const std::vector<uint8_t> &payload,
                                                yuan::net::mqtt::QoS,
                                                bool)
    {
        return dispatch_publish_topic(topic, payload);
    }

    bool ExampleMqttBusinessHandler::on_subscribe(yuan::net::mqtt::MqttSession *,
                                                  const std::string &topic_filter,
                                                  yuan::net::mqtt::QoS qos)
    {
        return dispatch_subscribe_topic(topic_filter, qos);
    }

    bool ExampleMqttBusinessHandler::dispatch_publish_topic(const std::string &topic,
                                                            const std::vector<uint8_t> &payload)
    {
        for (const auto &route : publish_routes_) {
            if (starts_with(topic, route.topic_prefix)) {
                return route.handler(topic, payload);
            }
        }
        return handle_default_publish(topic, payload);
    }

    bool ExampleMqttBusinessHandler::handle_default_publish(const std::string &topic,
                                                            const std::vector<uint8_t> &payload)
    {
        std::cout << "[biz][default] topic=" << topic << " payload_bytes=" << payload.size() << '\n';
        return true;
    }

    bool ExampleMqttBusinessHandler::dispatch_subscribe_topic(const std::string &topic_filter,
                                                              yuan::net::mqtt::QoS qos)
    {
        for (const auto &route : subscribe_routes_) {
            if (starts_with(topic_filter, route.topic_prefix)) {
                return route.handler(topic_filter, qos);
            }
        }
        return handle_default_subscribe(topic_filter, qos);
    }

    bool ExampleMqttBusinessHandler::handle_default_subscribe(const std::string &topic_filter,
                                                              yuan::net::mqtt::QoS)
    {
        std::cout << "[biz][subscribe] filter=" << topic_filter << " allow=true\n";
        return true;
    }
}
