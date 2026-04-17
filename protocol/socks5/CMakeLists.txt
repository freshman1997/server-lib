cmake_minimum_required(VERSION 3.12)
project(Socks5Proto)

FILE(GLOB_RECURSE SOCKS5_SRC "./src/*.cpp")

add_library(Socks5Proto ${SOCKS5_SRC})
target_link_libraries(Socks5Proto PUBLIC Core)
target_include_directories(Socks5Proto PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
