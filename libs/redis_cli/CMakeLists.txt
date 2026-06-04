cmake_minimum_required(VERSION 3.12)
project(redis_cli)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static")

include_directories(${CMAKE_CURRENT_LIST_DIR}/src/)

FILE(GLOB_RECURSE REDIS_CLI_SRC "./src/*.cpp")

add_library(redisCli ${REDIS_CLI_SRC})
target_link_libraries(redisCli PUBLIC Core)
target_include_directories(redisCli PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/src>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

if (YUAN_BUILD_TESTS)
    add_executable(redisCliParsingTest ./test/TestRedisParsing.cpp)
    target_link_libraries(redisCliParsingTest PUBLIC redisCli)
    add_test(NAME redis_cli_parsing COMMAND redisCliParsingTest)

    add_executable(redisCliE2ETest ./test/TestRedisE2E.cpp)
    target_link_libraries(redisCliE2ETest PUBLIC redisCli)
    add_test(NAME redis_cli_e2e COMMAND redisCliE2ETest)

    add_executable(redisCliStressTest ./test/TestRedisStress.cpp)
    target_link_libraries(redisCliStressTest PUBLIC redisCli)
    if (WIN32)
        target_link_libraries(redisCliStressTest PUBLIC psapi)
    endif()
    add_test(NAME redis_cli_stress COMMAND redisCliStressTest)
endif()

option(YREDIS_BUILD_CLI "Build the yredis CLI tool" ON)
if (YREDIS_BUILD_CLI)
    add_subdirectory(cli)
endif()

option(REDIS_CLI_BUILD_MANUAL_TESTS "Build interactive redis_cli smoke programs" OFF)
if (REDIS_CLI_BUILD_MANUAL_TESTS)
    add_executable(redisCliTest ./test/TestRedis.cpp)
    target_link_libraries(redisCliTest PUBLIC redisCli)

    add_executable(redisCliTest1 ./test/TestRedis1.cpp)
    target_link_libraries(redisCliTest1 PUBLIC redisCli)
endif()
