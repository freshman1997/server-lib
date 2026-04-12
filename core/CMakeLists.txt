cmake_minimum_required(VERSION 3.12)
project(core-modules)

if(NOT TARGET CoreBase)
    add_subdirectory(base)
endif()
add_subdirectory(core)
add_subdirectory(app)

if(YUAN_ENABLE_PLUGIN_SYSTEM)
    add_subdirectory(plugin_core)
    add_subdirectory(plugin_host)
endif()
