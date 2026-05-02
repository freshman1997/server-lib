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
set(YUAN_ENABLE_SSH_DEFAULT ON)
if (WIN32)
    set(YUAN_ENABLE_SSH_DEFAULT OFF)
endif()
option(YUAN_ENABLE_SSH "Enable SSH protocol support" ${YUAN_ENABLE_SSH_DEFAULT})
option(YUAN_ENABLE_SSH_SFTP "Enable SSH SFTP subsystem support" ${YUAN_ENABLE_SSH})

if (WIN32 AND YUAN_ENABLE_SSH)
    message(WARNING "SSH support is currently disabled on Windows because the module is not build-ready there yet.")
    set(YUAN_ENABLE_SSH OFF CACHE BOOL "Enable SSH protocol support" FORCE)
    set(YUAN_ENABLE_SSH_SFTP OFF CACHE BOOL "Enable SSH SFTP subsystem support" FORCE)
endif()
option(YUAN_FORCE_STATIC_LINK "Prefer static libraries and runtime" ON)
option(KCP_BUILD_TESTING "Build kcp tests" OFF)

if (YUAN_FORCE_STATIC_LINK)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
endif()

# Backward-compatible variables used by existing subdirectories.
set(HTTP_USE_SSL ${YUAN_ENABLE_HTTP_SSL})
set(WS_USE_SSL ${YUAN_ENABLE_WS_SSL})

add_library(webserver_project_options INTERFACE)
target_compile_features(webserver_project_options INTERFACE cxx_std_20)

# Keep global include directories for legacy module compatibility during migration.
include_directories(
    third_party/json/include/
    third_party/kcp/
    third_party/brotli/c/include/
    third_party/openssl-3.4.0/include/
    third_party/fmt/include/
)

add_subdirectory(third_party/kcp)
add_subdirectory(third_party/fmt)

set(BROTLI_BUILD_TOOLS OFF CACHE BOOL "Disable Brotli CLI tools" FORCE)
set(BROTLI_DISABLE_TESTS ON CACHE BOOL "Disable Brotli tests" FORCE)
add_subdirectory(third_party/brotli)

set(ZLIB_BUILD_TESTING OFF CACHE BOOL "Disable zlib tests" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "Disable zlib shared lib" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "Enable zlib static lib" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "Disable zlib install" FORCE)
add_subdirectory(third_party/zlib)

if (YUAN_FORCE_STATIC_LINK AND MINGW)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -static -static-libgcc -static-libstdc++")
endif()

add_subdirectory(core/base)
add_subdirectory(logger)
add_subdirectory(core)

if (YUAN_FORCE_STATIC_LINK)
    set_target_properties(kcp PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    if (TARGET fmt)
        set_target_properties(fmt PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    endif()
    if (TARGET fmtd)
        set_target_properties(fmtd PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    endif()
endif()

add_subdirectory(protocol/http)
add_subdirectory(protocol/ftp)
add_subdirectory(protocol/websocket)
add_subdirectory(protocol/bit_torrent)
add_subdirectory(protocol/dns)
add_subdirectory(protocol/socks5)
add_subdirectory(protocol/mqtt)
add_subdirectory(protocol/smb)
add_subdirectory(protocol/shadowsocks)
add_subdirectory(protocol/webdav)

if (YUAN_ENABLE_SSH)
    add_subdirectory(protocol/ssh)
endif()

add_subdirectory(release)

if (YUAN_BUILD_LIBS)
    add_subdirectory(libs)
endif()

if (YUAN_BUILD_SERVERS OR YUAN_BUILD_TESTS OR YUAN_BUILD_EXAMPLE)
    add_subdirectory(server/proxy)
    add_subdirectory(server/nas)
    add_subdirectory(server/services)
endif()

if (YUAN_BUILD_SERVERS)
    add_subdirectory(server)
endif()

if (YUAN_BUILD_TESTS)
    add_subdirectory(test)
endif()

if (YUAN_ENABLE_PLUGIN_SYSTEM)
    if (YUAN_FORCE_STATIC_LINK)
        set(YUAN_BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
        set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared libraries" FORCE)
    endif()

    add_subdirectory(plugins)

    if (YUAN_FORCE_STATIC_LINK)
        set(BUILD_SHARED_LIBS ${YUAN_BUILD_SHARED_LIBS_SAVED} CACHE BOOL "Build shared libraries" FORCE)
        unset(YUAN_BUILD_SHARED_LIBS_SAVED)
    endif()
endif()

if (YUAN_BUILD_EXAMPLE)
    add_executable(Test main.cpp)
    target_link_libraries(Test PUBLIC App Core DnsProto Logger ServerServices Socks5Proto webserver_project_options)
endif()
