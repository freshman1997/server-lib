# MQTT Protocol Module

This module provides MQTT server-side protocol support for MQTT v3.1.1 and v5.0.

## What Is Included

- Packet codec (`CONNECT`, `PUBLISH`, `SUBSCRIBE`, `UNSUBSCRIBE`, QoS acks, `DISCONNECT`, `AUTH`)
- Session manager with client-id binding and topic-alias tracking
- Topic tree with wildcard filter matching and shared subscription support
- Retained message store with wildcard matching and expiry cleanup
- Dispatcher for protocol state transitions and packet handling
- Server wrapper (`MqttServer`) and service integration (`MqttService`)
- Enhanced service handler (`MqttEnhancedHandler`) for auth/ACL/metrics defaults

## Current Feature Coverage

- Protocol versions: MQTT v3.1.1 and MQTT v5.0
- QoS: QoS 0/1/2 end-to-end handshake handling
- Retain: retained publish storage and replay on subscription
- Will: will publish on unexpected disconnect (with basic delay behavior)
- MQTT v5 features:
  - Topic Alias
  - Session Expiry Interval
  - Message Expiry Interval
  - Receive Maximum
  - User Property
  - Reason Code based negative responses
  - AUTH packet handling
- Shared subscriptions (`$share/<group>/<filter>`) with per-group delivery selection
- Retained store persistence (`save_to_file` / `load_from_file`)

## Service-Side Usage

Use `MqttService` when embedding as a reusable server service:

```cpp
#include "mqtt_service.h"
#include "mqtt.h"

class MyMqttHandler : public yuan::net::mqtt::MqttHandler {
public:
    bool on_connect(yuan::net::mqtt::MqttSession *,
                    const std::string &client_id,
                    const std::string &username,
                    const std::string &password) override {
        return true;
    }
};

int main() {
    yuan::net::mqtt::MqttServerConfig cfg;
    cfg.max_packet_size = 256 * 1024;
    cfg.idle_timeout_ms = 30000;

    yuan::server::MqttService svc(1883, cfg);

    // Optional: use built-in enhanced handler.
    svc.use_enhanced_handler(true);
    auto &eh = svc.enhanced_handler();
    eh.set_allow_anonymous(false);
    eh.upsert_user("device", "secret");
    eh.add_publish_acl("devices/+/telemetry", true);
    eh.add_publish_acl("#", false);

    // Or plug your own custom handler.
    // MyMqttHandler handler;
    // svc.set_handler(&handler);

    if (!svc.init()) {
        return 1;
    }
    (void)svc.load_retained_store("mqtt_retained_store.json");
    svc.start();

    // Before shutdown:
    // (void)svc.save_retained_store("mqtt_retained_store.json");
    return 0;
}
```

## Notes For Integrators

- `MqttServer::publish(...)` can be used for server-originated outbound publish.
- For QoS > 0 outbound paths, callers should rely on session/ack hooks in `MqttHandler`.
- Authentication policy can be implemented either by `MqttEnhancedHandler` or a custom `MqttHandler` (`on_connect` and `on_auth`).

## Tests

- Unit-style tests: `test/protocol/mqtt/test_mqtt.cpp`
- Feature/integration-style tests: `test/protocol/mqtt/test_mqtt_features.cpp`

## Release Delivery

- Release server entry: `release/mqtt/mqtt_server_main.cpp` (`ReleaseMqttServerApp`)
- Build target: `release_mqtt_server`
- Default config file: `release/mqtt/config.json`
- Runtime persistence defaults:
  - `mqtt_retained_store.json`
  - `mqtt_sessions.json`
  - `mqtt_policy.json`

Example run:

```bash
./release_mqtt_server --config release/mqtt/config.json
```
