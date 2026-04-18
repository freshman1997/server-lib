cmake_minimum_required(VERSION 3.12)
project(PluginLua)

file(GLOB_RECURSE PLUGIN_LUA_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(PluginLua ${PLUGIN_LUA_SRC})
target_compile_features(PluginLua PUBLIC cxx_std_20)
target_link_libraries(PluginLua PUBLIC PluginCore lua_static)
target_include_directories(PluginLua PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/src>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
