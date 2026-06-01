cmake_minimum_required(VERSION 3.12)
project(Core)

file(GLOB_RECURSE CORE_ALL_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(Core ${CORE_ALL_SRC})
target_compile_features(Core PUBLIC cxx_std_20)

target_link_directories(Core PUBLIC ../../third_party/openssl-3.4.0)

if(UNIX OR APPLE)
    target_link_libraries(Core PUBLIC CoreBase kcp ssl crypto dl -pthread Logger)
elseif(MINGW)
    target_link_libraries(Core PUBLIC CoreBase kcp ssl crypto ws2_32 crypt32 Logger)
elseif(WIN32)
    target_link_libraries(Core PUBLIC CoreBase kcp libssl libcrypto ws2_32 crypt32 Logger)
endif()

target_include_directories(Core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../app/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
