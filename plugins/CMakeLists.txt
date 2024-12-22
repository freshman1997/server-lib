cmake_minimum_required(VERSION 3.12)
project(plugins)

add_library(HelloWorld SHARED ./helloworld/helloworld.cpp)
target_link_libraries(HelloWorld PUBLIC Core)
