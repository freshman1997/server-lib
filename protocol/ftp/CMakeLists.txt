cmake_minimum_required(VERSION 3.12)
project(FtpProto)

FILE(GLOB_RECURSE FTP_SRC "./src/*.cpp")

add_library(FtpProto ${FTP_SRC})
target_link_libraries(FtpProto PUBLIC Core)
target_include_directories(FtpProto PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
