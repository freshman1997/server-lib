cmake_minimum_required(VERSION 3.12)
project(ServerServices)

file(GLOB_RECURSE SERVER_SERVICES_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(ServerServices ${SERVER_SERVICES_SRC})
target_compile_features(ServerServices PUBLIC cxx_std_20)
target_link_libraries(ServerServices PUBLIC App DnsProto HttpProto FtpProto WebSocketProto BitTorrentProto Socks5Proto SmbProto MqttProto SshProto)

target_include_directories(ServerServices PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
