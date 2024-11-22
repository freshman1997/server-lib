cmake_minimum_required(VERSION 3.12)
project(HttpProto)

FILE(GLOB_RECURSE HTTP_SRC "./src/*.cpp")

add_library(HttpProto ${HTTP_SRC})
target_link_libraries(HttpProto PUBLIC base)
target_include_directories(HttpProto PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
