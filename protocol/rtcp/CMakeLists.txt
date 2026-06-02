cmake_minimum_required(VERSION 3.12)
project(RtcpProto)

file(GLOB_RECURSE RTCP_SRC "./src/*.cpp")

add_library(RtcpProto ${RTCP_SRC})
target_link_libraries(RtcpProto PUBLIC Core RtcProto)
target_include_directories(RtcpProto PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
