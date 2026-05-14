cmake_minimum_required(VERSION 3.12)
project(test)
include(CTest)
enable_testing()
include_directories("${CMAKE_CURRENT_SOURCE_DIR}")

option(YUAN_TEST_BUILD_CORE "Build test/core targets" ON)
option(YUAN_TEST_BUILD_NAS "Build test/nas targets" ON)
option(YUAN_TEST_BUILD_NETWORK "Build test/network targets" ON)
option(YUAN_TEST_BUILD_PLUGIN "Build test/plugin targets" ON)
option(YUAN_TEST_BUILD_PROTOCOL "Build test/protocol targets" ON)
option(YUAN_TEST_BUILD_PROXY "Build test/proxy targets" ON)
option(YUAN_TEST_BUILD_BENCHMARK "Build test/benchmark targets" ON)

if (YUAN_TEST_BUILD_CORE)
    add_subdirectory(core)
endif()

if (YUAN_TEST_BUILD_NAS)
    add_subdirectory(nas)
endif()

if (YUAN_TEST_BUILD_NETWORK)
    add_subdirectory(network)
endif()

if (YUAN_TEST_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()

if (YUAN_TEST_BUILD_PROTOCOL)
    add_subdirectory(protocol)
endif()

if (YUAN_TEST_BUILD_PROXY)
    add_subdirectory(proxy)
endif()

if (YUAN_TEST_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif()
