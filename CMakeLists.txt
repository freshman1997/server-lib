cmake_minimum_required(VERSION 3.12)
project(webserver)
set(CMAKE_CXX_STANDARD 17)
include_directories(
    include/
    third_party/json/include/
    third_party/kcp/
)

FILE(GLOB_RECURSE SERVER_SRC "./src/*.cpp")

add_subdirectory(third_party/kcp)

add_executable(server ${SERVER_SRC} main.cpp)
add_executable(http_server ${SERVER_SRC} test/test_http_server.cpp)
add_executable(http_client ${SERVER_SRC} test/test_http_client.cpp)
add_executable(ftp_server ${SERVER_SRC} test/test_ftp_server.cpp)
add_executable(ftp_client ${SERVER_SRC} test/test_ftp_client.cpp)

if(UNIX OR APPLE)
    target_link_libraries(server PUBLIC pthread kcp)
    target_link_libraries(http_server PUBLIC pthread kcp)
    target_link_libraries(http_client PUBLIC pthread kcp)
    target_link_libraries(ftp_server PUBLIC pthread kcp)
    target_link_libraries(ftp_client PUBLIC pthread kcp)
elseif(WIN32)
    target_link_libraries(server PUBLIC wsock32 kcp)
    target_link_libraries(http_server PUBLIC wsock32 kcp)
    target_link_libraries(http_client PUBLIC wsock32 kcp)
    target_link_libraries(ftp_server PUBLIC wsock32 kcp)
    target_link_libraries(ftp_client PUBLIC wsock32 kcp)
endif()
