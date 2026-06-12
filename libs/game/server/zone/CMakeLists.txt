file(GLOB GAME_SERVER_ZONE_SOURCES CONFIGURE_DEPENDS "*_service.cpp")
add_library(GameServerZone ${GAME_SERVER_ZONE_SOURCES})
target_compile_features(GameServerZone PUBLIC cxx_std_20)
target_include_directories(GameServerZone PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerZone PUBLIC GameServerTunnel)

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    file(GLOB GAME_ZONE_PROCESS_SOURCES CONFIGURE_DEPENDS "*_server_process.cpp")
    add_executable(game_zone_server_process ${GAME_ZONE_PROCESS_SOURCES})
    target_link_libraries(game_zone_server_process PUBLIC GameServerZone)
endif()
