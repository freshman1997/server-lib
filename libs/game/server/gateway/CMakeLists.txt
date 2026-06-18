file(GLOB GAME_SERVER_GATEWAY_SOURCES CONFIGURE_DEPENDS "model/*.cpp" "rpc/*.cpp" "app/*_server_service.cpp")
add_library(GameServerGateway ${GAME_SERVER_GATEWAY_SOURCES})
target_compile_features(GameServerGateway PUBLIC cxx_std_20)
target_include_directories(GameServerGateway PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerGateway PUBLIC GameServerCommon GameServerMessaging)

file(GLOB GAME_GATEWAY_SERVER_SOURCES CONFIGURE_DEPENDS "app/*_server.cpp")
add_executable(game_gateway_server ${GAME_GATEWAY_SERVER_SOURCES})
target_link_libraries(game_gateway_server PUBLIC GameServerGateway)
