cmake_minimum_required(VERSION 3.12)
project(Core)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static -fPIC")

FILE(GLOB_RECURSE CORE_SRC "./src/*.cpp")

add_library(Core ${CORE_SRC})

target_link_directories(Core PUBLIC ../../third_party/openssl-3.4.0)

if(UNIX OR APPLE)
    target_link_libraries(Core PUBLIC kcp ssl crypto pthread dl)
elseif(MINGW)
    target_link_libraries(Core PUBLIC kcp ssl crypto wsock32 ws2_32 crypt32)
elseif(WIN32)
    target_link_libraries(Core PUBLIC kcp libssl libcrypto wsock32 ws2_32 crypt32)
endif()

target_include_directories(Core PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
