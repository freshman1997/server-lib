file(GLOB GAME_SERVER_ZONE_SOURCES CONFIGURE_DEPENDS "*_service.cpp")
add_library(GameServerZone ${GAME_SERVER_ZONE_SOURCES})
target_compile_features(GameServerZone PUBLIC cxx_std_20)
target_include_directories(GameServerZone PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerZone PUBLIC GameServerCommon GameServerMessaging redisCli)

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    file(GLOB GAME_ZONE_SERVER_SOURCES CONFIGURE_DEPENDS "*_server.cpp")
    list(FILTER GAME_ZONE_SERVER_SOURCES EXCLUDE REGEX "_server_service\\.cpp$")
    add_executable(game_zone_server ${GAME_ZONE_SERVER_SOURCES})
    target_link_libraries(game_zone_server PUBLIC GameServerZone)
endif()
