cmake_minimum_required(VERSION 3.12)
project(redis_cli)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static")

include_directories(${CMAKE_CURRENT_LIST_DIR}/src/)

FILE(GLOB_RECURSE REDIS_CLI_SRC "./src/*.cpp")

add_library(redisCli ${REDIS_CLI_SRC})
target_link_libraries(redisCli PUBLIC Core)
target_include_directories(redisCli PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/src>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

add_executable(redisCliTest ./test/TestRedis.cpp)
target_link_libraries(redisCliTest PUBLIC redisCli)
add_executable(redisCliTest1 ./test/TestRedis1.cpp)
target_link_libraries(redisCliTest1 PUBLIC redisCli)
