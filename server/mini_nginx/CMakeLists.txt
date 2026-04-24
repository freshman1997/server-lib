cmake_minimum_required(VERSION 3.12)
project(mini_nginx)

add_executable(mini_nginx main.cpp)
target_link_libraries(mini_nginx PUBLIC App ServerServices)
