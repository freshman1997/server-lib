#include "mqtt_business_routes.h"

#include "mqtt_business_example.h"
#include "mqtt_business_options.h"

#include <iostream>

namespace yuan::release::mqtt
{
    namespace
    {
        bool handle_order_publish(const std::string &topic, const std::vector<uint8_t> &payload)
        {
            std::cout << "[biz][order] topic=" << topic << " payload_bytes=" << payload.size() << '\n';
            return true;
        }

        bool handle_device_publish(const std::string &topic, const std::vector<uint8_t> &payload)
        {
            std::cout << "[biz][device] topic=" << topic << " payload_bytes=" << payload.size() << '\n';
            return true;
        }

        bool handle_event_publish(const std::string &topic, const std::vector<uint8_t> &payload)
        {
            std::cout << "[biz][event] topic=" << topic << " payload_bytes=" << payload.size() << '\n';
            return true;
        }

        bool handle_deny_publish(const std::string &topic, const std::vector<uint8_t> &)
        {
            std::cout << "[biz] reject publish topic=" << topic << '\n';
            return false;
        }

        bool handle_private_subscribe_deny(const std::string &topic_filter, yuan::net::mqtt::QoS)
        {
            std::cout << "[biz] reject subscribe filter=" << topic_filter << '\n';
            return false;
        }

        bool handle_shared_subscribe_audit(const std::string &topic_filter, yuan::net::mqtt::QoS)
        {
            std::cout << "[biz][subscribe] shared filter=" << topic_filter << " allow=true\n";
            return true;
        }
    }

    void register_example_publish_routes(ExampleMqttBusinessHandler &handler,
                                         const MqttBusinessRouteOptions &options)
    {
        if (options.enable_publish_deny_route) {
            handler.register_publish_route("biz/deny/", handle_deny_publish);
        }
        if (options.enable_publish_order_route) {
            handler.register_publish_route("biz/order/", handle_order_publish);
        }
        if (options.enable_publish_device_route) {
            handler.register_publish_route("biz/device/", handle_device_publish);
        }
        if (options.enable_publish_event_route) {
            handler.register_publish_route("biz/event/", handle_event_publish);
        }
    }

    void register_example_subscribe_routes(ExampleMqttBusinessHandler &handler,
                                           const MqttBusinessRouteOptions &options)
    {
        if (options.enable_subscribe_private_deny_route) {
            handler.register_subscribe_route("biz/private/", handle_private_subscribe_deny);
        }
        if (options.enable_subscribe_shared_audit_route) {
            handler.register_subscribe_route("$share/", handle_shared_subscribe_audit);
        }
    }
}
