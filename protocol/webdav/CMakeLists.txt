cmake_minimum_required(VERSION 3.12)
project(WebDavProto)

file(GLOB_RECURSE WEBDAV_SRC "./src/*.cpp")

add_library(WebDavProto ${WEBDAV_SRC})
target_link_libraries(WebDavProto PUBLIC HttpProto)
target_include_directories(WebDavProto
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
