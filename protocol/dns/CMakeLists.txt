cmake_minimum_required(VERSION 3.12)
project(DnsProto)

include_directories(
    include/
    include/
    third_party/json/include/
    third_party/kcp/
)

FILE(GLOB_RECURSE DNS_SRC "./src/*.cpp")

add_library(DnsProto ${DNS_SRC})
target_link_libraries(DnsProto PUBLIC base)
