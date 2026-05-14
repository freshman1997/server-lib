cmake_minimum_required(VERSION 3.12)
project(RtcProto)

file(GLOB_RECURSE RTC_SRC "./src/*.cpp")

add_library(RtcProto STATIC ${RTC_SRC})
target_link_libraries(RtcProto PUBLIC Core)
target_include_directories(RtcProto PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
