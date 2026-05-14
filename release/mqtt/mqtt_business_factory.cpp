#include "mqtt_business_factory.h"

#include "mqtt_business_example.h"

namespace yuan::release::mqtt
{
    std::unique_ptr<IMqttBusinessHandler> create_mqtt_business_handler(MqttBusinessRouteOptions options)
    {
        return std::make_unique<ExampleMqttBusinessHandler>(options);
    }
}
