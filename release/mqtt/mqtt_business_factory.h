#ifndef __RELEASE_MQTT_BUSINESS_FACTORY_H__
#define __RELEASE_MQTT_BUSINESS_FACTORY_H__

#include "mqtt_business_handler.h"
#include "mqtt_business_options.h"

#include <memory>

namespace yuan::release::mqtt
{
    std::unique_ptr<IMqttBusinessHandler> create_mqtt_business_handler(
        MqttBusinessRouteOptions options = {});
}

#endif
