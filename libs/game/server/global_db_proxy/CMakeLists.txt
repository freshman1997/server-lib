file(GLOB GAME_SERVER_GLOBAL_DB_PROXY_SOURCES CONFIGURE_DEPENDS
    "app/*_service.cpp"
    "rpc/*.cpp")
add_library(GameServerGlobalDbProxy ${GAME_SERVER_GLOBAL_DB_PROXY_SOURCES})
target_compile_features(GameServerGlobalDbProxy PUBLIC cxx_std_20)
target_include_directories(GameServerGlobalDbProxy PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerGlobalDbProxy PUBLIC GameServerCommon GameServerMessaging redisCli)

file(GLOB GAME_GLOBAL_DB_PROXY_SERVER_SOURCES CONFIGURE_DEPENDS "app/*_server.cpp")
add_executable(game_global_db_proxy_server ${GAME_GLOBAL_DB_PROXY_SERVER_SOURCES})
target_link_libraries(game_global_db_proxy_server PUBLIC GameServerGlobalDbProxy)
