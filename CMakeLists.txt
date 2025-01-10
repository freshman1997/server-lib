cmake_minimum_required(VERSION 3.12)
project(server-lib)
set(CMAKE_CXX_STANDARD 20)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static")

include_directories(
    third_party/json/include/
    third_party/kcp/
    third_party/openssl-3.4.0/include/
)

# 修改 http 可启用ssl ssl/ca.crt ssl/ca.key
set(HTTP_USE_SSL OFF)

# 修改 websocket 可启用ssl ssl/ca.crt ssl/ca.key
set(WS_USE_SSL OFF)

add_subdirectory(third_party/kcp)
add_subdirectory(core)
add_subdirectory(protocol/http)
add_subdirectory(protocol/ftp)
add_subdirectory(protocol/dns)
add_subdirectory(protocol/bit_torrent)
add_subdirectory(protocol/websocket)
add_subdirectory(test)
add_subdirectory(plugins)

add_executable(Test main.cpp)
target_link_libraries(Test PUBLIC Core HttpProto DnsProto FtpProto)
