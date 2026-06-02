cmake_minimum_required(VERSION 3.12)
project(WebrtcProto)

file(GLOB_RECURSE WEBRTC_SRC "./src/*.cpp")

add_library(WebrtcProto ${WEBRTC_SRC})
target_link_libraries(WebrtcProto PUBLIC Core RtcProto RtcpProto)
target_include_directories(WebrtcProto PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
