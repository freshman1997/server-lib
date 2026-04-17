cmake_minimum_required(VERSION 3.12)
project(PluginCore)

file(GLOB_RECURSE PLUGIN_CORE_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(PluginCore ${PLUGIN_CORE_SRC})
target_compile_features(PluginCore PUBLIC cxx_std_20)
target_link_libraries(PluginCore PUBLIC CoreBase Logger)
target_include_directories(PluginCore PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

if(UNIX OR APPLE)
    target_link_libraries(PluginCore PUBLIC dl)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9")
    target_link_libraries(PluginCore PUBLIC stdc++fs)
endif()
