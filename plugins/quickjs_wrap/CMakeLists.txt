cmake_minimum_required(VERSION 3.12)
project(quickjs)

enable_language(C)

set(QJS_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/quickjs/quickjs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/quickjs/dtoa.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/quickjs/libregexp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/quickjs/libunicode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/quickjs/cutils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/quickjs/quickjs-libc.c
)

add_library(quickjs_static STATIC ${QJS_SRC})

set(QUICKJS_VERSION "2025-09-13")

target_include_directories(quickjs_static PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_include_directories(quickjs_static PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/quickjs"
)

set_target_properties(quickjs_static PROPERTIES
    OUTPUT_NAME quickjs
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

if(UNIX AND NOT MINGW)
    target_link_libraries(quickjs_static PUBLIC dl m Threads::Threads)
elseif(MINGW)
    target_link_libraries(quickjs_static PUBLIC Threads::Threads)
endif()

target_compile_options(quickjs_static PRIVATE
    -fwrapv
    -D_GNU_SOURCE
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-unused-parameter
    -Wno-infinite-recursion
    -Wno-array-bounds
    -Wno-format-truncation
)

target_compile_definitions(quickjs_static PUBLIC
    CONFIG_VERSION=\"${QUICKJS_VERSION}\"
)
