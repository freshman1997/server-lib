cmake_minimum_required(VERSION 3.12)
project(DnsProto)

FILE(GLOB_RECURSE DNS_SRC "./src/*.cpp")

add_library(DnsProto ${DNS_SRC})
target_link_libraries(DnsProto PUBLIC base)
target_include_directories(DnsProto PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
