file(GLOB GAME_SERVER_GLOBAL_SOURCES CONFIGURE_DEPENDS "rpc/*.cpp" "app/*_server_service.cpp")
add_library(GameServerGlobal ${GAME_SERVER_GLOBAL_SOURCES})
target_compile_features(GameServerGlobal PUBLIC cxx_std_20)
target_include_directories(GameServerGlobal PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerGlobal PUBLIC GameServerCommon GameServerMessaging)

file(GLOB GAME_GLOBAL_SERVER_SOURCES CONFIGURE_DEPENDS "app/*_server.cpp")
add_executable(game_global_server ${GAME_GLOBAL_SERVER_SOURCES})
target_link_libraries(game_global_server PUBLIC GameServerGlobal)
