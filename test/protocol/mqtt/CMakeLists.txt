add_executable(test_mqtt test_mqtt.cpp)
target_link_libraries(test_mqtt PUBLIC MqttProto)

add_executable(test_mqtt_features test_mqtt_features.cpp)
target_link_libraries(test_mqtt_features PUBLIC MqttProto ServerServiceMqtt)

add_executable(test_mqtt_service_enhanced test_mqtt_service_enhanced.cpp)
target_link_libraries(test_mqtt_service_enhanced PUBLIC MqttProto ServerServiceMqtt)

if (BUILD_TESTING)
    add_test(NAME mqtt_unit COMMAND test_mqtt)
    add_test(NAME mqtt_features COMMAND test_mqtt_features)
    add_test(NAME mqtt_service_enhanced COMMAND test_mqtt_service_enhanced)
endif()
