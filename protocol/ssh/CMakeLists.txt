cmake_minimum_required(VERSION 3.12)
project(SshProto)

find_package(ZLIB QUIET)

FILE(GLOB_RECURSE SSH_SRC "./src/*.cpp")

if (NOT YUAN_ENABLE_SSH_SFTP)
    list(FILTER SSH_SRC EXCLUDE REGEX ".*/src/sftp/.*\\.cpp$")
endif()

if (NOT ZLIB_FOUND)
    list(FILTER SSH_SRC EXCLUDE REGEX ".*/src/algorithm/compression/ssh_compression_zlib\\.cpp$")
endif()

add_library(SshProto ${SSH_SRC})
target_link_libraries(SshProto PUBLIC Core)
if (ZLIB_FOUND)
    target_link_libraries(SshProto PUBLIC ZLIB::ZLIB)
endif()
target_include_directories(SshProto PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_definitions(SshProto PUBLIC
    YUAN_ENABLE_SSH=$<IF:$<BOOL:${YUAN_ENABLE_SSH}>,1,0>
    YUAN_ENABLE_SSH_SFTP=$<IF:$<BOOL:${YUAN_ENABLE_SSH_SFTP}>,1,0>
    YUAN_SSH_HAS_ZLIB=$<IF:$<BOOL:${ZLIB_FOUND}>,1,0>)
