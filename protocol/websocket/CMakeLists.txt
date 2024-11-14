cmake_minimum_required(VERSION 3.12)
project(WebSocketProto)
include_directories(
    protocol/http/include
)

FILE(GLOB_RECURSE WEB_SOCKET_SRC "./*.cpp")

add_library(WebSocketProto ${WEB_SOCKET_SRC})
target_link_libraries(WebSocketProto PUBLIC base HttpProto ssl crypto)
