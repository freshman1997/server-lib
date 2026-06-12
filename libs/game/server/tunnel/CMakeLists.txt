file(GLOB GAME_SERVER_TUNNEL_SOURCES CONFIGURE_DEPENDS "*_service.cpp")
add_library(GameServerTunnel ${GAME_SERVER_TUNNEL_SOURCES})
target_compile_features(GameServerTunnel PUBLIC cxx_std_20)
target_include_directories(GameServerTunnel PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerTunnel PUBLIC GameServerCommon)

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    file(GLOB GAME_TUNNEL_PROCESS_SOURCES CONFIGURE_DEPENDS "*_server_process.cpp")
    add_executable(game_tunnel_server_process ${GAME_TUNNEL_PROCESS_SOURCES})
    target_link_libraries(game_tunnel_server_process PUBLIC GameServerTunnel)
endif()
