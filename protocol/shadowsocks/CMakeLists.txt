cmake_minimum_required(VERSION 3.12)
project(ShadowsocksProto)

FILE(GLOB_RECURSE SHADOWSOCKS_SRC "./src/*.cpp")

add_library(ShadowsocksProto ${SHADOWSOCKS_SRC})
target_link_libraries(ShadowsocksProto PUBLIC Core)
target_include_directories(ShadowsocksProto PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
