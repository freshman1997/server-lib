cmake_minimum_required(VERSION 3.12)
project(ServerServices)

file(GLOB_RECURSE SERVER_SERVICES_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER SERVER_SERVICES_SRC EXCLUDE REGEX "proxy_service\.cpp$")
list(FILTER SERVER_SERVICES_SRC EXCLUDE REGEX "server_runtime_host\.cpp$")

if (NOT TARGET FtpProto)
    list(FILTER SERVER_SERVICES_SRC EXCLUDE REGEX "ftp_service\\.cpp$")
endif()
if (NOT TARGET SmbProto)
    list(FILTER SERVER_SERVICES_SRC EXCLUDE REGEX "smb_service\\.cpp$")
endif()

add_library(ServerServices ${SERVER_SERVICES_SRC})
target_compile_features(ServerServices PUBLIC cxx_std_20)
target_link_libraries(ServerServices PUBLIC ServerProxy DnsProto HttpProto WebSocketProto BitTorrentProto Socks5Proto MqttProto SshProto)

if (TARGET FtpProto)
    target_link_libraries(ServerServices PUBLIC FtpProto)
endif()

if (TARGET SmbProto)
    target_link_libraries(ServerServices PUBLIC SmbProto)
endif()

target_include_directories(ServerServices PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
