cmake_minimum_required(VERSION 3.12)
project(server-lib)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static")

include_directories(
    third_party/json/include/
    third_party/kcp/
    third_party/openssl-3.4.0/include/
)

# 修改 http 可启用ssl ssl/ca.crt ssl/ca.key
set(HTTP_USE_SSL OFF)

# 修改 websocket 可启用ssl ssl/ca.crt ssl/ca.key
set(WS_USE_SSL OFF)

FILE(GLOB_RECURSE SERVER_SRC "./src/*.cpp")

add_subdirectory(third_party/kcp)
add_library(base ${SERVER_SRC})
target_include_directories(base PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

add_subdirectory(protocol/http)
add_subdirectory(protocol/ftp)
add_subdirectory(protocol/dns)
add_subdirectory(protocol/bit_torrent)
add_subdirectory(protocol/websocket)
add_subdirectory(test)

add_executable(server main.cpp)

target_link_directories(base PUBLIC third_party/openssl-3.4.0)

if(UNIX OR APPLE)
    target_link_libraries(base PUBLIC kcp ssl crypto pthread dl)
elseif(MINGW)
    target_link_libraries(base PUBLIC kcp ssl crypto wsock32 ws2_32 crypt32)
elseif(WIN32)
    target_link_libraries(base PUBLIC kcp libssl libcrypto wsock32 ws2_32 crypt32)
endif()

target_link_libraries(server PUBLIC base HttpProto DnsProto FtpProto)
