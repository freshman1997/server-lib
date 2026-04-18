cmake_minimum_required(VERSION 3.12)
project(PluginTypeScript)

file(GLOB_RECURSE PLUGIN_TYPESCRIPT_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(PluginTypeScript ${PLUGIN_TYPESCRIPT_SRC})
target_compile_features(PluginTypeScript PUBLIC cxx_std_20)
target_link_libraries(PluginTypeScript PUBLIC PluginCore quickjs_static)
target_include_directories(PluginTypeScript PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/src>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
