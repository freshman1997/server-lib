#ifndef __RELEASE_MQTT_BUSINESS_OPTIONS_H__
#define __RELEASE_MQTT_BUSINESS_OPTIONS_H__

namespace yuan::release::mqtt
{
    struct MqttBusinessRouteOptions
    {
        bool enable_publish_deny_route = true;
        bool enable_publish_order_route = true;
        bool enable_publish_device_route = true;
        bool enable_publish_event_route = true;
        bool enable_subscribe_private_deny_route = true;
        bool enable_subscribe_shared_audit_route = true;
    };
}

#endif
