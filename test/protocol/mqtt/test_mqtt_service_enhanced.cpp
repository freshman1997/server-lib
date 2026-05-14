#include "mqtt_service.h"

#include <cstdio>
#include <iostream>
#include <string>

using namespace yuan::server;
using namespace yuan::net::mqtt;

static int g_run = 0;
static int g_failed = 0;

#define CHECK(expr, msg)                                  \
    do {                                                  \
        if (!(expr)) {                                    \
            std::cout << "FAIL: " << msg << std::endl;    \
            ++g_failed;                                   \
        }                                                 \
    } while (0)

#define RUN(t)                \
    do {                      \
        ++g_run;              \
        std::cout << #t "\n"; \
        t();                  \
    } while (0)

static void test_enhanced_handler_auth_and_acl()
{
    MqttEnhancedHandler handler;
    handler.set_allow_anonymous(false);
    handler.upsert_user("u", "p");
    handler.set_default_publish_allow(false);
    handler.set_default_subscribe_allow(false);
    handler.add_publish_acl("devices/+/telemetry", true);
    handler.add_subscribe_acl("devices/#", true);

    CHECK(!handler.on_connect(nullptr, "c1", "", ""), "anonymous should be rejected");
    CHECK(handler.on_connect(nullptr, "c1", "u", "p"), "known user should pass");
    CHECK(!handler.on_connect(nullptr, "c1", "u", "bad"), "bad password should reject");

    CHECK(handler.on_publish(nullptr, "devices/a/telemetry", {}, QoS::AT_MOST_ONCE, false), "publish acl allow");
    CHECK(!handler.on_publish(nullptr, "devices/a/private", {}, QoS::AT_MOST_ONCE, false), "publish acl deny by default");

    CHECK(handler.on_subscribe(nullptr, "devices/#", QoS::AT_LEAST_ONCE), "subscribe acl allow");
    CHECK(!handler.on_subscribe(nullptr, "secret/#", QoS::AT_LEAST_ONCE), "subscribe acl deny by default");

    const auto &m = handler.metrics();
    CHECK(m.connect_attempts.load() >= 3, "connect attempts should count");
    CHECK(m.publish_allowed.load() >= 1, "publish allowed should count");
    CHECK(m.publish_denied.load() >= 1, "publish denied should count");
}

static void test_mqtt_service_retained_persistence_api()
{
    MqttService service(0, MqttServerConfig{});
    const std::string path = "mqtt_service_retained.json";

    CHECK(!service.save_retained_store(""), "empty path save should fail");
    CHECK(!service.load_retained_store("__not_exists__.json"), "loading non-existing file should fail");

    (void)std::remove(path.c_str());
}

int main()
{
    RUN(test_enhanced_handler_auth_and_acl);
    RUN(test_mqtt_service_retained_persistence_api);

    std::cout << "mqtt service enhanced tests: " << (g_run - g_failed) << "/" << g_run << std::endl;
    return g_failed == 0 ? 0 : 1;
}
