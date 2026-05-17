cmake_minimum_required(VERSION 3.12)
project(SshProto)

set(YUAN_SSH_ZLIB_AVAILABLE OFF)
if (TARGET zlibstatic OR TARGET zlib)
    set(YUAN_SSH_ZLIB_AVAILABLE ON)
else()
    find_package(ZLIB QUIET)
    if (ZLIB_FOUND)
        set(YUAN_SSH_ZLIB_AVAILABLE ON)
    endif()
endif()

FILE(GLOB_RECURSE SSH_SRC "./src/*.cpp")

if (NOT YUAN_ENABLE_SSH_SFTP)
    list(FILTER SSH_SRC EXCLUDE REGEX ".*/src/sftp/.*\\.cpp$")
endif()

if (NOT YUAN_SSH_ZLIB_AVAILABLE)
    list(FILTER SSH_SRC EXCLUDE REGEX ".*/src/algorithm/compression/ssh_compression_zlib\\.cpp$")
endif()

add_library(SshProto ${SSH_SRC})
target_link_libraries(SshProto PUBLIC Core)
if (UNIX AND NOT APPLE)
    find_library(YUAN_UTIL_LIBRARY util)
    if (YUAN_UTIL_LIBRARY)
        target_link_libraries(SshProto PUBLIC ${YUAN_UTIL_LIBRARY})
    endif()
endif()
if (TARGET zlibstatic)
    target_link_libraries(SshProto PUBLIC zlibstatic)
elseif(TARGET zlib)
    target_link_libraries(SshProto PUBLIC zlib)
elseif (ZLIB_FOUND)
    target_link_libraries(SshProto PUBLIC ZLIB::ZLIB)
endif()
target_include_directories(SshProto PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_definitions(SshProto PUBLIC
    YUAN_ENABLE_SSH=$<IF:$<BOOL:${YUAN_ENABLE_SSH}>,1,0>
    YUAN_ENABLE_SSH_SFTP=$<IF:$<BOOL:${YUAN_ENABLE_SSH_SFTP}>,1,0>
    YUAN_SSH_HAS_ZLIB=$<IF:$<BOOL:${YUAN_SSH_ZLIB_AVAILABLE}>,1,0>)
