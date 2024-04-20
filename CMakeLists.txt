cmake_minimum_required(VERSION 3.12)
project(webserver)
set(CMAKE_CXX_STANDARD 17)
include_directories(
    include/
    third_party/json/include/
)

FILE(GLOB_RECURSE SERVER_SRC "./src/*.cpp")

add_executable(server ${SERVER_SRC} main.cpp)


if(UNIX)
target_link_libraries(server PUBLIC pthread)
else()
target_link_libraries(server PUBLIC wsock32)
endif()