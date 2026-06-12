file(GLOB GAME_SERVER_MESSAGING_SOURCES CONFIGURE_DEPENDS "*.cpp")
add_library(GameServerMessaging ${GAME_SERVER_MESSAGING_SOURCES})
target_compile_features(GameServerMessaging PUBLIC cxx_std_20)
target_include_directories(GameServerMessaging PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerMessaging PUBLIC GameServerCommon)
