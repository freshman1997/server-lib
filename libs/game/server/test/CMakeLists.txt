find_package(Threads REQUIRED)

file(GLOB GAME_SERVER_TEST_SOURCES CONFIGURE_DEPENDS "test_*.cpp")
foreach(GAME_SERVER_TEST_SOURCE IN LISTS GAME_SERVER_TEST_SOURCES)
    get_filename_component(GAME_SERVER_TEST_TARGET "${GAME_SERVER_TEST_SOURCE}" NAME_WE)
    string(REPLACE "test_" "" GAME_SERVER_TEST_NAME "${GAME_SERVER_TEST_TARGET}")
    add_executable(${GAME_SERVER_TEST_TARGET} "${GAME_SERVER_TEST_SOURCE}")
    target_link_libraries(${GAME_SERVER_TEST_TARGET} PUBLIC GameServerLib Threads::Threads)
    if(BUILD_TESTING AND NOT GAME_SERVER_TEST_TARGET STREQUAL "test_game_server_process_smoke")
        add_test(NAME ${GAME_SERVER_TEST_NAME} COMMAND ${GAME_SERVER_TEST_TARGET})
    endif()
endforeach()

target_compile_definitions(test_game_server_process_smoke PRIVATE GAME_SERVER_CONFIG_DIR="${GAME_SERVER_CONFIG_OUTPUT_DIR}")
add_dependencies(test_game_server_process_smoke game_global_server_process game_tunnel_server_process game_zone_server_process copy_game_server_configs)
if(BUILD_TESTING)
    add_test(NAME game_server_process_smoke
        COMMAND test_game_server_process_smoke
                $<TARGET_FILE:game_global_server_process>
                $<TARGET_FILE:game_tunnel_server_process>
                $<TARGET_FILE:game_zone_server_process>)
endif()
