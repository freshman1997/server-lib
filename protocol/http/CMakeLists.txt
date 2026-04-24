cmake_minimum_required(VERSION 3.12)
project(HttpProto)

if(HTTP_USE_SSL)
    add_compile_definitions(HTTP_USE_SSL)
endif()

FILE(GLOB_RECURSE HTTP_SRC "./src/*.cpp")

add_library(HttpProto STATIC ${HTTP_SRC})
target_link_libraries(HttpProto PUBLIC Core)

if (TARGET brotlicommon AND TARGET brotlienc)
    target_link_libraries(HttpProto PUBLIC brotlienc brotlicommon)
    target_compile_definitions(HttpProto PUBLIC YUAN_HTTP_HAS_BROTLI=1)
else()
    target_compile_definitions(HttpProto PUBLIC YUAN_HTTP_HAS_BROTLI=0)
endif()

if (TARGET ZLIB::ZLIBSTATIC)
    target_link_libraries(HttpProto PUBLIC ZLIB::ZLIBSTATIC)
    target_compile_definitions(HttpProto PUBLIC YUAN_HTTP_HAS_ZLIB=1)
elseif (TARGET ZLIB::ZLIB)
    target_link_libraries(HttpProto PUBLIC ZLIB::ZLIB)
    target_compile_definitions(HttpProto PUBLIC YUAN_HTTP_HAS_ZLIB=1)
else()
    target_compile_definitions(HttpProto PUBLIC YUAN_HTTP_HAS_ZLIB=0)
endif()

target_include_directories(HttpProto PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
