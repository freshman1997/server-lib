file(GLOB GAME_SERVER_WORLD_DB_PROXY_SOURCES CONFIGURE_DEPENDS
    "app/*_service.cpp"
    "rpc/*.cpp")
add_library(GameServerWorldDbProxy ${GAME_SERVER_WORLD_DB_PROXY_SOURCES})
target_compile_features(GameServerWorldDbProxy PUBLIC cxx_std_20)
target_include_directories(GameServerWorldDbProxy PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerWorldDbProxy PUBLIC GameServerCommon GameServerMessaging redisCli)

file(GLOB GAME_WORLD_DB_PROXY_SERVER_SOURCES CONFIGURE_DEPENDS "app/*_server.cpp")
add_executable(game_world_db_proxy_server ${GAME_WORLD_DB_PROXY_SERVER_SOURCES})
target_link_libraries(game_world_db_proxy_server PUBLIC GameServerWorldDbProxy)
