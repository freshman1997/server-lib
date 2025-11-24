cmake_minimum_required(VERSION 3.12)
project(HttpProto)

if(HTTP_USE_SSL)
    add_compile_definitions(HTTP_USE_SSL)
endif()

FILE(GLOB_RECURSE HTTP_SRC "./src/*.cpp")

add_library(HttpProto ${HTTP_SRC}
        include/task/save_upload_tmp_chunk_task.h
        include/define/upload.h
        src/task/save_upload_tmp_chunk_task.cpp)
target_link_libraries(HttpProto PUBLIC Core)
target_include_directories(HttpProto PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
