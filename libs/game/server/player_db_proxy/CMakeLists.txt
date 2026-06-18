file(GLOB GAME_SERVER_PLAYER_DB_PROXY_SOURCES CONFIGURE_DEPENDS
    "app/*_service.cpp"
    "rpc/*.cpp")
add_library(GameServerPlayerDbProxy ${GAME_SERVER_PLAYER_DB_PROXY_SOURCES})
target_compile_features(GameServerPlayerDbProxy PUBLIC cxx_std_20)
target_include_directories(GameServerPlayerDbProxy PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerPlayerDbProxy PUBLIC GameServerCommon GameServerMessaging redisCli)

file(GLOB GAME_PLAYER_DB_PROXY_SERVER_SOURCES CONFIGURE_DEPENDS "app/*_server.cpp")
add_executable(game_player_db_proxy_server ${GAME_PLAYER_DB_PROXY_SERVER_SOURCES})
target_link_libraries(game_player_db_proxy_server PUBLIC GameServerPlayerDbProxy)
