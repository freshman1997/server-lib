cmake_minimum_required(VERSION 3.12)
project(match_server)

set(CMAKE_CXX_STANDARD 20)

# 设置包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/json/include/
    ${CMAKE_SOURCE_DIR}/third_party/kcp/
    ${CMAKE_SOURCE_DIR}/third_party/openssl-3.4.0/include/
    ${CMAKE_SOURCE_DIR}/third_party/fmt/include/
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 收集源文件
FILE(GLOB_RECURSE MATCH_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# 创建可执行文件
add_executable(match_server ${MATCH_SRC} main.cpp)

# 链接库
target_link_libraries(match_server 
    PUBLIC 
    Core 
    Logger
    fmt::fmt
)

# 设置输出目录
set_target_properties(match_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 复制配置文件到输出目录
add_custom_command(TARGET match_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/config"
        "${CMAKE_BINARY_DIR}/bin/config"
)
