cmake_minimum_required(VERSION 3.12)
project(GameServer)

add_subdirectory(common)
add_subdirectory(messaging)
add_subdirectory(tunnel)
add_subdirectory(global)
add_subdirectory(zone)
add_subdirectory(world)
add_subdirectory(gateway)
add_subdirectory(web)
add_subdirectory(rank)
add_subdirectory(chat_web)
add_subdirectory(player_db_proxy)
add_subdirectory(world_db_proxy)
add_subdirectory(global_db_proxy)

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    add_subdirectory(client)
endif()

set(GAME_SERVER_CONFIG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/config")
add_custom_target(copy_game_server_configs ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GAME_SERVER_CONFIG_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_LIST_DIR}/config" "${GAME_SERVER_CONFIG_OUTPUT_DIR}")

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    add_subdirectory(test)
endif()
