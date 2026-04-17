cmake_minimum_required(VERSION 3.12)
project(MqttProto)

FILE(GLOB_RECURSE MQTT_SRC "./src/*.cpp")

add_library(MqttProto ${MQTT_SRC})
target_link_libraries(MqttProto PUBLIC Core)
target_include_directories(MqttProto PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
