file(GLOB GAME_SERVER_GLOBAL_SOURCES CONFIGURE_DEPENDS "*_service.cpp")
add_library(GameServerGlobal ${GAME_SERVER_GLOBAL_SOURCES})
target_compile_features(GameServerGlobal PUBLIC cxx_std_20)
target_include_directories(GameServerGlobal PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerGlobal PUBLIC GameServerCommon GameServerMessaging)

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    file(GLOB GAME_GLOBAL_SERVER_SOURCES CONFIGURE_DEPENDS "*_server.cpp")
    list(FILTER GAME_GLOBAL_SERVER_SOURCES EXCLUDE REGEX "_server_service\\.cpp$")
    add_executable(game_global_server ${GAME_GLOBAL_SERVER_SOURCES})
    target_link_libraries(game_global_server PUBLIC GameServerGlobal)
endif()
