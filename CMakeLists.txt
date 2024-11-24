cmake_minimum_required(VERSION 3.12)
project(server-lib)
set(CMAKE_CXX_STANDARD 17)

include_directories(
    third_party/json/include/
    third_party/kcp/
)

if (MINGW)
    add_compile_options("-static")
endif()

if (MSVC)

elseif(UNIX)
    add_compile_options(-static)
elseif(APPLE)
    add_compile_options(-static)
endif()

# 查找 OpenSSL 库
find_package(OpenSSL REQUIRED)

# 添加 OpenSSL 头文件目录
include_directories(${OPENSSL_INCLUDE_DIR})

FILE(GLOB_RECURSE SERVER_SRC "./src/*.cpp")

add_subdirectory(third_party/kcp)
add_library(base ${SERVER_SRC})
target_include_directories(base PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

add_subdirectory(protocol/http)
add_subdirectory(protocol/ftp)
add_subdirectory(protocol/dns)
add_subdirectory(protocol/bit_torrent)
add_subdirectory(protocol/websocket)
add_subdirectory(test)

add_executable(server main.cpp)

if(UNIX OR APPLE)
    target_link_libraries(base PUBLIC pthread kcp ${OPENSSL_LIBRARIES})
elseif(WIN32)
    target_link_libraries(base PUBLIC wsock32 kcp)
endif()

target_link_libraries(server PUBLIC base HttpProto DnsProto FtpProto)
