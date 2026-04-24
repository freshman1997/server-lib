# MQTT Review and Test Progress Log

Updated: 2026-04-24

## Scope

- Review MQTT design and implementation correctness.
- Identify and fix protocol and runtime defects.
- Build server-client integration testing capability.
- Execute a full-feature test matrix and record outcomes.

## Code Fixes Completed

### Protocol codec and dispatcher

- Added MQTT v5 property decoding in `decode_publish`.
  - `protocol/mqtt/include/mqtt_codec.h`
  - `protocol/mqtt/src/mqtt_codec.cpp`
- Fixed packet boundary handling in dispatcher. Dispatcher now parses fixed header and forwards packet body to decode handlers.
  - `protocol/mqtt/src/mqtt_dispatcher.cpp`
- Fixed Topic Alias handling order for alias-only publish in v5.
  - `protocol/mqtt/src/mqtt_dispatcher.cpp`
- Fixed `try_decode` false-positive on incomplete packets (`total > len` check).
  - `protocol/mqtt/src/mqtt_codec.cpp`

### Properties and session management

- Fixed uint32 property encoding endianness to network order.
- Fixed variable-byte-int decode offset progression bug.
  - `protocol/mqtt/src/mqtt_properties.cpp`
- Added stable client-id index binding in session manager.
- Added fallback index repair logic in `find_by_client_id`.
  - `protocol/mqtt/include/mqtt_session.h`
  - `protocol/mqtt/src/mqtt_session.cpp`
  - `protocol/mqtt/src/mqtt_dispatcher.cpp`

### Server runtime safety

- Added max packet size enforcement in read loop.
- Added v5 DISCONNECT with `PACKET_TOO_LARGE` when applicable.
  - `protocol/mqtt/src/mqtt_server.cpp`

## Test Infrastructure Added

- Extended unit tests for codec/dispatcher/property behavior:
  - `test/protocol/mqtt/test_mqtt.cpp`
- Added integration feature test executable:
  - `test/protocol/mqtt/test_mqtt_features.cpp`
- Added CMake and CTest targets:
  - `test/protocol/mqtt/CMakeLists.txt`
  - Targets: `test_mqtt`, `test_mqtt_features`
  - CTest names: `mqtt_unit`, `mqtt_features`

## Feature Matrix Status

### A. Connect / Session / Auth

- v3.1.1 connect success
- v5 connect success with credentials path
- auth reject path returns expected connack/disconnect behavior
- duplicate client-id replacement closes old connection
- unsupported protocol version rejection

Status: Done

### B. PubSub / QoS / Retain / Will / Alias

- QoS0 publish delivery
- QoS1 publish ack (`PUBACK`) with packet-id validation
- retained message replay after subscribe
- will publish on ungraceful disconnect
- v5 topic-alias mapping and alias-only publish path

Status: Done

### C. Invalid / Boundary / Compatibility

- packet before CONNECT is rejected (connection closed)
- v5 AUTH default reject path
- invalid topic filter reason code check in SUBACK
- oversized packet path with v5 `PACKET_TOO_LARGE`

Status: Done

## Current Results

- `test_mqtt`: 66/66 passed
- `test_mqtt_features`: passed
- `ctest -R "mqtt_(unit|features)"`: 2/2 passed

## Known Follow-up Items

- Add external broker/client interoperability checks (for example paho or mosquitto clients).
- Expand QoS2 long-run and reconnection scenarios.
- Add persistent session recovery tests for clean-start=false paths.

## Resume Guide

When continuing later, start with:

1. Build and run MQTT tests:
   - `cmake --build E:/test/server-lib/build --target test_mqtt test_mqtt_features -j 4`
   - `ctest --test-dir E:/test/server-lib/build -R "mqtt_(unit|features)" --output-on-failure`
2. Continue from interoperability and persistence scenarios in "Known Follow-up Items".
