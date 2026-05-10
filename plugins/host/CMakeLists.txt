cmake_minimum_required(VERSION 3.12)
project(PluginHost)

file(GLOB_RECURSE PLUGIN_HOST_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

if(YUAN_FORCE_STATIC_LINK AND MINGW)
    add_library(PluginHost STATIC ${PLUGIN_HOST_SRC})
else()
    add_library(PluginHost ${PLUGIN_HOST_SRC})
endif()
target_compile_features(PluginHost PUBLIC cxx_std_20)
target_link_libraries(PluginHost PUBLIC App PluginCore Logger redisCli PluginLua PluginTypeScript)
target_include_directories(PluginHost PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
