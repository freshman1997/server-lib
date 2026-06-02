cmake_minimum_required(VERSION 3.12)
project(RtspProto)

file(GLOB_RECURSE RTSP_SRC "./src/*.cpp")

add_library(RtspProto ${RTSP_SRC})
target_link_libraries(RtspProto PUBLIC Core RtcProto RtcpProto)
target_include_directories(RtspProto PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
