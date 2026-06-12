file(GLOB GAME_SERVER_WEB_SOURCES CONFIGURE_DEPENDS "*_service.cpp")
add_library(GameServerWeb ${GAME_SERVER_WEB_SOURCES})
target_compile_features(GameServerWeb PUBLIC cxx_std_20)
target_include_directories(GameServerWeb PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerWeb PUBLIC GameServerCommon redisCli)

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    file(GLOB GAME_WEB_SERVER_SOURCES CONFIGURE_DEPENDS "*_server.cpp")
    list(FILTER GAME_WEB_SERVER_SOURCES EXCLUDE REGEX "_server_service\\.cpp$")
    if(GAME_WEB_SERVER_SOURCES)
        add_executable(game_web_server ${GAME_WEB_SERVER_SOURCES})
        target_link_libraries(game_web_server PUBLIC GameServerWeb)
    endif()
endif()
