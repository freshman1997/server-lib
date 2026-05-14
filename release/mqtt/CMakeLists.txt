cmake_minimum_required(VERSION 3.12)
project(release_mqtt)

set(RELEASE_MQTT_OUTPUT_DIR "${CMAKE_BINARY_DIR}/release/mqtt")

add_executable(release_mqtt_server
    mqtt_business_example.cpp
    mqtt_business_factory.cpp
    mqtt_business_handler.cpp
    mqtt_business_routes.cpp
    mqtt_server_app.cpp
    mqtt_server_main.cpp
)
target_compile_features(release_mqtt_server PUBLIC cxx_std_20)
target_link_libraries(release_mqtt_server PUBLIC Core MqttProto ServerServices)
target_include_directories(release_mqtt_server PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/protocol/mqtt/include"
    "${CMAKE_SOURCE_DIR}/server/services/include"
)
if (UNIX AND NOT APPLE)
    target_link_options(release_mqtt_server PRIVATE "-static-libgcc" "-static-libstdc++")
endif()
set_target_properties(release_mqtt_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_MQTT_OUTPUT_DIR}"
)

if (WIN32)
    target_link_libraries(release_mqtt_server PUBLIC ws2_32)
endif()

add_custom_command(
    TARGET release_mqtt_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
            "${RELEASE_MQTT_OUTPUT_DIR}/config.json"
    COMMENT "Copy release mqtt default config"
)
