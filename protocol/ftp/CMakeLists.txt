cmake_minimum_required(VERSION 3.12)
project(FtpProto)

include_directories(
    include/
    include/
    third_party/json/include/
    third_party/kcp/
)

FILE(GLOB_RECURSE FTP_SRC "./src/*.cpp")

add_library(FtpProto ${FTP_SRC})
target_link_libraries(FtpProto PUBLIC base)
