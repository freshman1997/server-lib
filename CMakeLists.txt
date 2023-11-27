cmake_minimum_required(VERSION 3.12)
project(webserver)

include_directories(
    include/
    third_party/json/include/
)

FILE(GLOB_RECURSE SERVER_SRC "./src/*.cpp")

add_executable(server ${SERVER_SRC} main.cpp)
