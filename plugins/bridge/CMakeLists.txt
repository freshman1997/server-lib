cmake_minimum_required(VERSION 3.12)
project(PluginBridge)

if(YUAN_FORCE_STATIC_LINK AND MINGW)
    add_library(PluginBridge STATIC src/plugin_http_bridge.cpp)
else()
    add_library(PluginBridge src/plugin_http_bridge.cpp)
endif()
target_compile_features(PluginBridge PUBLIC cxx_std_20)
target_link_libraries(PluginBridge PUBLIC PluginHost ServerServices HttpProto)
target_include_directories(PluginBridge PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
