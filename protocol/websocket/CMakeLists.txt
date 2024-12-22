cmake_minimum_required(VERSION 3.12)
project(WebSocketProto)

if(WS_USE_SSL)
    add_compile_definitions(WS_USE_SSL)
endif()

FILE(GLOB_RECURSE WEB_SOCKET_SRC "./*.cpp")

add_library(WebSocketProto ${WEB_SOCKET_SRC})
target_link_libraries(WebSocketProto PUBLIC Core HttpProto)
target_include_directories(WebSocketProto PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
