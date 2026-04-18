cmake_minimum_required(VERSION 3.12)
project(lua54)

set(LUA_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/lua-5.4.8/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/lua-5.4.8/src)

set(src_code
        ${LUA_SRC_DIR}/lapi.c ${LUA_SRC_DIR}/lcode.c ${LUA_SRC_DIR}/lctype.c ${LUA_SRC_DIR}/ldebug.c
        ${LUA_SRC_DIR}/ldo.c ${LUA_SRC_DIR}/ldump.c ${LUA_SRC_DIR}/lfunc.c ${LUA_SRC_DIR}/lgc.c
        ${LUA_SRC_DIR}/llex.c ${LUA_SRC_DIR}/lmem.c ${LUA_SRC_DIR}/lobject.c ${LUA_SRC_DIR}/lopcodes.c
        ${LUA_SRC_DIR}/lparser.c ${LUA_SRC_DIR}/lstate.c ${LUA_SRC_DIR}/lstring.c ${LUA_SRC_DIR}/ltable.c
        ${LUA_SRC_DIR}/ltm.c ${LUA_SRC_DIR}/lundump.c ${LUA_SRC_DIR}/lvm.c ${LUA_SRC_DIR}/lzio.c)
set(src_lib
        ${LUA_SRC_DIR}/lauxlib.c ${LUA_SRC_DIR}/lbaselib.c ${LUA_SRC_DIR}/lcorolib.c ${LUA_SRC_DIR}/ldblib.c
        ${LUA_SRC_DIR}/liolib.c ${LUA_SRC_DIR}/lmathlib.c ${LUA_SRC_DIR}/loadlib.c ${LUA_SRC_DIR}/loslib.c
        ${LUA_SRC_DIR}/lstrlib.c ${LUA_SRC_DIR}/ltablib.c ${LUA_SRC_DIR}/lutf8lib.c ${LUA_SRC_DIR}/linit.c)

add_library(lua_static STATIC ${src_code} ${src_lib})

target_include_directories(lua_static PUBLIC
    "$<BUILD_INTERFACE:${LUA_SRC_DIR}>"
    "$<BUILD_INTERFACE:${LUA_SRC_DIR}/include>"
)

set_target_properties(lua_static PROPERTIES
    OUTPUT_NAME Lua
    POSITION_INDEPENDENT_CODE ON
)

if(UNIX)
    target_link_libraries(lua_static PRIVATE dl)
endif()
