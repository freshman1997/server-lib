file(GLOB GAME_SERVER_COMMON_SOURCES CONFIGURE_DEPENDS "*.cpp" "codec/*.cpp" "storage/*.cpp")
add_library(GameServerCommon ${GAME_SERVER_COMMON_SOURCES})
target_compile_features(GameServerCommon PUBLIC cxx_std_20)
target_include_directories(GameServerCommon PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerCommon PUBLIC GameBase YuanRpc Core App redisCli)
