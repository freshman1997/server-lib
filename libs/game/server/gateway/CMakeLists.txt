file(GLOB GAME_SERVER_GATEWAY_SOURCES CONFIGURE_DEPENDS "*_service.cpp")
add_library(GameServerGateway ${GAME_SERVER_GATEWAY_SOURCES})
target_compile_features(GameServerGateway PUBLIC cxx_std_20)
target_include_directories(GameServerGateway PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerGateway PUBLIC GameServerCommon GameServerMessaging)

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    file(GLOB GAME_GATEWAY_SERVER_SOURCES CONFIGURE_DEPENDS "*_server.cpp")
    list(FILTER GAME_GATEWAY_SERVER_SOURCES EXCLUDE REGEX "_server_service\\.cpp$")
    if(GAME_GATEWAY_SERVER_SOURCES)
        add_executable(game_gateway_server ${GAME_GATEWAY_SERVER_SOURCES})
        target_link_libraries(game_gateway_server PUBLIC GameServerGateway)
    endif()
endif()
