cmake_minimum_required(VERSION 3.12)
project(server-lib)
set(CMAKE_CXX_STANDARD 17)
include_directories(
    include/
    protocol/http/include
    protocol/ftp/include
    protocol/dns/include
    protocol/bit_torrent/include
    third_party/json/include/
    third_party/kcp/
)

FILE(GLOB_RECURSE SERVER_SRC "./src/*.cpp")

add_subdirectory(third_party/kcp)
add_library(base ${SERVER_SRC})

add_subdirectory(protocol/http)
add_subdirectory(protocol/ftp)
add_subdirectory(protocol/dns)
add_subdirectory(protocol/bit_torrent)

add_executable(http_server test/test_http_server.cpp)
add_executable(http_client test/test_http_client.cpp)
add_executable(ftp_server test/test_ftp_server.cpp)
add_executable(ftp_client test/test_ftp_client.cpp)

add_executable(test main.cpp)

if(UNIX OR APPLE)
    target_link_libraries(server PUBLIC pthread kcp)
    
elseif(WIN32)
    target_link_libraries(base PUBLIC wsock32 kcp)
endif()

target_link_libraries(http_server PUBLIC HttpProto)
target_link_libraries(http_client PUBLIC HttpProto)
target_link_libraries(ftp_server PUBLIC FtpProto)
target_link_libraries(ftp_client PUBLIC FtpProto)

target_link_libraries(test PUBLIC base HttpProto DnsProto)
