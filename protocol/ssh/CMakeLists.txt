cmake_minimum_required(VERSION 3.12)
project(SshProto)

FILE(GLOB_RECURSE SSH_SRC "./src/*.cpp")

add_library(SshProto ${SSH_SRC})
target_link_libraries(SshProto PUBLIC Core)
target_include_directories(SshProto PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
