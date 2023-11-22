cmake_minimum_required(VERSION 3.12)
project(webserver)

include_directories(
    include/
)



add_executable(server main.cpp)
