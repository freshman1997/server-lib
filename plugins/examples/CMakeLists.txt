cmake_minimum_required(VERSION 3.12)
project(plugin-examples)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(HelloWorld SHARED ${CMAKE_CURRENT_SOURCE_DIR}/helloworld/helloworld.cpp)
target_link_libraries(HelloWorld PRIVATE App PluginCore)

add_library(LineEchoProtocol SHARED ${CMAKE_CURRENT_SOURCE_DIR}/line_echo/line_echo.cpp)
target_link_libraries(LineEchoProtocol PRIVATE App PluginCore)

set(targets HelloWorld LineEchoProtocol)

foreach(target ${targets})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${target}
        PREFIX ""
        SUFFIX ".plugin"
    )
endforeach(target ${targets})

add_custom_command(TARGET LineEchoProtocol POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/line_echo/plugin.json
            $<TARGET_FILE_DIR:LineEchoProtocol>/LineEchoProtocol.json
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:LineEchoProtocol>/lua_line_echo
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/lua_line_echo/plugin.json
            $<TARGET_FILE_DIR:LineEchoProtocol>/lua_line_echo/plugin.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/lua_line_echo/main.lua
            $<TARGET_FILE_DIR:LineEchoProtocol>/lua_line_echo/main.lua
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/lua_line_echo/README.md
            $<TARGET_FILE_DIR:LineEchoProtocol>/lua_line_echo/README.md
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:LineEchoProtocol>/ts_line_echo
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/ts_line_echo/plugin.json
            $<TARGET_FILE_DIR:LineEchoProtocol>/ts_line_echo/plugin.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/ts_line_echo/main.ts
            $<TARGET_FILE_DIR:LineEchoProtocol>/ts_line_echo/main.ts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/ts_line_echo/README.md
            $<TARGET_FILE_DIR:LineEchoProtocol>/ts_line_echo/README.md)
