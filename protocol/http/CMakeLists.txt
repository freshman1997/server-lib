cmake_minimum_required(VERSION 3.12)
project(HttpProto)

include_directories(
    ../include/
    include/
    third_party/json/include/
    third_party/kcp/
)

FILE(GLOB_RECURSE HTTP_SRC "./src/*.cpp")

add_library(HttpProto ${HTTP_SRC})
target_link_libraries(HttpProto PUBLIC base)
