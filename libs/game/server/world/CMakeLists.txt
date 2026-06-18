file(GLOB GAME_SERVER_WORLD_SOURCES CONFIGURE_DEPENDS "model/*.cpp" "rpc/*.cpp" "app/*_server_service.cpp")
add_library(GameServerWorld ${GAME_SERVER_WORLD_SOURCES})
target_compile_features(GameServerWorld PUBLIC cxx_std_20)
target_include_directories(GameServerWorld PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerWorld PUBLIC GameServerCommon GameServerMessaging redisCli HttpProto)

file(GLOB GAME_WORLD_SERVER_SOURCES CONFIGURE_DEPENDS "app/*_server.cpp")
add_executable(game_world_server ${GAME_WORLD_SERVER_SOURCES})
target_link_libraries(game_world_server PUBLIC GameServerWorld)
