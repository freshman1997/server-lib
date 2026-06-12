file(GLOB GAME_SERVER_GLOBAL_SOURCES CONFIGURE_DEPENDS "*_service.cpp")
add_library(GameServerGlobal ${GAME_SERVER_GLOBAL_SOURCES})
target_compile_features(GameServerGlobal PUBLIC cxx_std_20)
target_include_directories(GameServerGlobal PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerGlobal PUBLIC GameServerTunnel)

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    file(GLOB GAME_GLOBAL_PROCESS_SOURCES CONFIGURE_DEPENDS "*_server_process.cpp")
    add_executable(game_global_server_process ${GAME_GLOBAL_PROCESS_SOURCES})
    target_link_libraries(game_global_server_process PUBLIC GameServerGlobal)
endif()
