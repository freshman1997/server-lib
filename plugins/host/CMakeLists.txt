cmake_minimum_required(VERSION 3.12)
project(PluginHost)

file(GLOB_RECURSE PLUGIN_HOST_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(PluginHost ${PLUGIN_HOST_SRC})
target_compile_features(PluginHost PUBLIC cxx_std_20)
target_link_libraries(PluginHost PUBLIC App PluginCore Logger redisCli)
target_include_directories(PluginHost PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
