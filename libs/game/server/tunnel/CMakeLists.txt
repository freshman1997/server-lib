file(GLOB GAME_SERVER_TUNNEL_SOURCES CONFIGURE_DEPENDS "rpc/*.cpp" "app/*_server_service.cpp")
add_library(GameServerTunnel ${GAME_SERVER_TUNNEL_SOURCES})
target_compile_features(GameServerTunnel PUBLIC cxx_std_20)
target_include_directories(GameServerTunnel PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerTunnel PUBLIC GameServerCommon GameServerMessaging)

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    file(GLOB GAME_TUNNEL_SERVER_SOURCES CONFIGURE_DEPENDS "app/*_server.cpp")
    add_executable(game_tunnel_server ${GAME_TUNNEL_SERVER_SOURCES})
    target_link_libraries(game_tunnel_server PUBLIC GameServerTunnel)
endif()
