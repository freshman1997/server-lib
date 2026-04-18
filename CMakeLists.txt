cmake_minimum_required(VERSION 3.16)

project(
    webserver
    VERSION 0.1.0
    LANGUAGES CXX
)

include(CTest)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(YUAN_BUILD_TESTS "Build test targets" ON)
option(YUAN_BUILD_PLUGINS "Build plugin targets" ON)
option(YUAN_BUILD_LIBS "Build optional libraries" ON)
option(YUAN_BUILD_SERVERS "Build server targets" ON)
option(YUAN_BUILD_EXAMPLE "Build the example executable" ON)
option(YUAN_ENABLE_HTTP_SSL "Enable HTTP SSL support" OFF)
option(YUAN_ENABLE_WS_SSL "Enable WebSocket SSL support" OFF)
option(YUAN_ENABLE_PLUGIN_SYSTEM "Build and integrate the plugin system" ${YUAN_BUILD_PLUGINS})
option(YUAN_ENABLE_SSH "Enable SSH protocol support" ON)
option(YUAN_ENABLE_SSH_SFTP "Enable SSH SFTP subsystem support" ON)

# Backward-compatible variables used by existing subdirectories.
set(HTTP_USE_SSL ${YUAN_ENABLE_HTTP_SSL})
set(WS_USE_SSL ${YUAN_ENABLE_WS_SSL})

add_library(webserver_project_options INTERFACE)
target_compile_features(webserver_project_options INTERFACE cxx_std_20)

# Keep global include directories for legacy module compatibility during migration.
include_directories(
    third_party/json/include/
    third_party/kcp/
    third_party/openssl-3.4.0/include/
    third_party/fmt/include/
)

add_subdirectory(third_party/kcp)
add_subdirectory(third_party/fmt)
add_subdirectory(core/base)
add_subdirectory(logger)
add_subdirectory(core)
add_subdirectory(protocol/http)
add_subdirectory(protocol/ftp)
add_subdirectory(protocol/websocket)
add_subdirectory(protocol/bit_torrent)
add_subdirectory(protocol/dns)
add_subdirectory(protocol/socks5)
add_subdirectory(protocol/mqtt)
add_subdirectory(protocol/smb)

if (YUAN_ENABLE_SSH)
    add_subdirectory(protocol/ssh)
endif()

if (YUAN_BUILD_SERVERS OR YUAN_BUILD_TESTS OR YUAN_BUILD_EXAMPLE)
    add_subdirectory(server/services)
endif()

if (YUAN_BUILD_SERVERS)
    add_subdirectory(server)
endif()

if (YUAN_BUILD_TESTS)
    add_subdirectory(test)
endif()

if (YUAN_ENABLE_PLUGIN_SYSTEM)
    add_subdirectory(plugins)
endif()

if (YUAN_BUILD_LIBS)
    add_subdirectory(libs)
endif()

if (YUAN_BUILD_EXAMPLE)
    add_executable(Test main.cpp)
    target_link_libraries(Test PUBLIC App Core DnsProto Logger ServerServices Socks5Proto webserver_project_options)
endif()
