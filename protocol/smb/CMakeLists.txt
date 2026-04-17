cmake_minimum_required(VERSION 3.12)
project(SmbProto)

FILE(GLOB_RECURSE SMB_SRC "./src/*.cpp")

add_library(SmbProto ${SMB_SRC})
target_link_libraries(SmbProto PUBLIC Core)
target_include_directories(SmbProto PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
