cmake_minimum_required(VERSION 3.12)
project(YuanRpc)

file(GLOB YUAN_RPC_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
add_library(YuanRpc ${YUAN_RPC_SOURCES})
target_compile_features(YuanRpc PUBLIC cxx_std_20)
target_include_directories(YuanRpc PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../../core/core/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

target_link_libraries(YuanRpc PUBLIC Core)

if(YUAN_ENABLE_TRACY)
    target_compile_definitions(YuanRpc PUBLIC YUAN_ENABLE_TRACY=1)
    target_link_libraries(YuanRpc PUBLIC Tracy::TracyClient)
endif()

add_library(yuan_rpc ALIAS YuanRpc)

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    add_executable(test_rpc_wire_protocol test/test_rpc_wire_protocol.cpp)
    target_link_libraries(test_rpc_wire_protocol PUBLIC YuanRpc)
    add_executable(test_rpc_local_bus test/test_rpc_local_bus.cpp)
    target_link_libraries(test_rpc_local_bus PUBLIC YuanRpc)
    if(BUILD_TESTING)
        add_test(NAME rpc_wire_protocol COMMAND test_rpc_wire_protocol)
        add_test(NAME rpc_local_bus COMMAND test_rpc_local_bus)
    endif()
endif()
