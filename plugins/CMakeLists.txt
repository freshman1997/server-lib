cmake_minimum_required(VERSION 3.12)
project(plugins)

add_subdirectory(core)
add_subdirectory(lua_wrap)
add_subdirectory(lua)

add_subdirectory(quickjs_wrap)
add_subdirectory(typescript)

add_subdirectory(host)
add_subdirectory(bridge)

add_subdirectory(examples)
