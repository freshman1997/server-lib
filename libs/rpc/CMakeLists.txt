cmake_minimum_required(VERSION 3.12)
project(yuan_rpc)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static")

include_directories(${CMAKE_CURRENT_LIST_DIR}/src/)

FILE(GLOB_RECURSE YUAN_RPC_SRC "./src/*.cpp")

add_library(yuan_rpc ${YUAN_RPC_SRC})
target_link_libraries(yuan_rpc PUBLIC Core)
target_include_directories(yuan_rpc PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/src>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

#add_executable(redisCliTest ./test/TestRedis.cpp)
#target_link_libraries(redisCliTest PUBLIC redisCli)
#add_executable(redisCliTest1 ./test/TestRedis1.cpp)
#target_link_libraries(redisCliTest1 PUBLIC redisCli)
