cmake_minimum_required(VERSION 3.12)
project(server-lib)
set(CMAKE_CXX_STANDARD 17)

include_directories(
    include/
    third_party/json/include/
    third_party/kcp/
    protocol/http/include
    protocol/ftp/include
    protocol/dns/include
    protocol/bit_torrent/include
)

FILE(GLOB_RECURSE SERVER_SRC "./src/*.cpp")

add_subdirectory(third_party/kcp)
add_library(base ${SERVER_SRC})

add_subdirectory(protocol/http)
add_subdirectory(protocol/ftp)
add_subdirectory(protocol/dns)
add_subdirectory(protocol/bit_torrent)
add_subdirectory(test)

add_executable(server main.cpp)

if(UNIX OR APPLE)
    target_link_libraries(base PUBLIC pthread kcp)
elseif(WIN32)
    target_link_libraries(base PUBLIC wsock32 kcp)
endif()

target_link_libraries(server PUBLIC base HttpProto DnsProto)
