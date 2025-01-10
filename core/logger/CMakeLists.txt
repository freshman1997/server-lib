cmake_minimum_required(VERSION 3.12)
project(Logger)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static -fPIC")

FILE(GLOB_RECURSE LOG_SRC "./src/*.cpp")

add_library(Logger ${LOG_SRC})

target_link_libraries(Logger PUBLIC Core)

target_include_directories(Logger PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
