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


if(UNIX OR APPLE)
    target_link_libraries(server PUBLIC pthread kcp)
elseif(WIN32)
    target_link_libraries(server PUBLIC wsock32 kcp)
endif()