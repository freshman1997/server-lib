add_library(GameServerCommon INTERFACE)
target_compile_features(GameServerCommon INTERFACE cxx_std_20)
target_include_directories(GameServerCommon INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GameServerCommon INTERFACE GameBase YuanRpc App)
