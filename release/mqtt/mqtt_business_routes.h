#ifndef __RELEASE_MQTT_BUSINESS_ROUTES_H__
#define __RELEASE_MQTT_BUSINESS_ROUTES_H__

namespace yuan::release::mqtt
{
    class ExampleMqttBusinessHandler;
    struct MqttBusinessRouteOptions;

    void register_example_publish_routes(ExampleMqttBusinessHandler &handler,
                                         const MqttBusinessRouteOptions &options);
    void register_example_subscribe_routes(ExampleMqttBusinessHandler &handler,
                                           const MqttBusinessRouteOptions &options);
}

#endif
