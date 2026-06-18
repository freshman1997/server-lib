file(GLOB GAME_SERVER_RANK_SOURCES CONFIGURE_DEPENDS "app/*_service.cpp" "rpc/*.cpp")
add_library(GameServerRank ${GAME_SERVER_RANK_SOURCES})
target_compile_features(GameServerRank PUBLIC cxx_std_20)
target_include_directories(GameServerRank PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerRank PUBLIC GameServerCommon GameServerMessaging redisCli)

file(GLOB GAME_RANK_SERVER_SOURCES CONFIGURE_DEPENDS "app/*_server.cpp")
add_executable(game_rank_server ${GAME_RANK_SERVER_SOURCES})
target_link_libraries(game_rank_server PUBLIC GameServerRank)
