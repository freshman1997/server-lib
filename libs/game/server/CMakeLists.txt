cmake_minimum_required(VERSION 3.12)
project(GameServerLib)

add_subdirectory(common)
add_subdirectory(tunnel)
add_subdirectory(global)
add_subdirectory(zone)

add_library(GameServerLib game_server.h)
target_compile_features(GameServerLib PUBLIC cxx_std_20)
target_include_directories(GameServerLib PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerLib PUBLIC
    GameServerCommon
    GameServerTunnel
    GameServerGlobal
    GameServerZone)

set(GAME_SERVER_CONFIG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/config")
add_custom_target(copy_game_server_configs ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GAME_SERVER_CONFIG_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_LIST_DIR}/config" "${GAME_SERVER_CONFIG_OUTPUT_DIR}")

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    add_subdirectory(test)
endif()
