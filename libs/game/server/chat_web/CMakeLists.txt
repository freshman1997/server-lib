file(GLOB GAME_SERVER_CHAT_WEB_SOURCES CONFIGURE_DEPENDS
    "app/*_service.cpp"
    "handler/*.cpp"
    "service/*.cpp")
add_library(GameServerChatWeb ${GAME_SERVER_CHAT_WEB_SOURCES})
target_compile_features(GameServerChatWeb PUBLIC cxx_std_20)
target_include_directories(GameServerChatWeb PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerChatWeb PUBLIC GameServerCommon redisCli HttpProto)

file(GLOB GAME_CHAT_WEB_SERVER_SOURCES CONFIGURE_DEPENDS "app/*_server.cpp")
add_executable(game_chat_web_server ${GAME_CHAT_WEB_SERVER_SOURCES})
target_link_libraries(game_chat_web_server PUBLIC GameServerChatWeb)
