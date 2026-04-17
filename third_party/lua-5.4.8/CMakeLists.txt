project(lua548)
cmake_minimum_required(VERSION 3.12)

set(src_code
        src/lapi.c src/lcode.c src/lctype.c src/ldebug.c src/ldo.c src/ldump.c src/lfunc.c src/lgc.c src/llex.c
        src/lmem.c src/lobject.c src/lopcodes.c src/lparser.c src/lstate.c src/lstring.c src/ltable.c src/ltm.c
        src/lundump.c src/lvm.c src/lzio.c)
set(src_lib
        src/lauxlib.c src/lbaselib.c src/lcorolib.c src/ldblib.c src/liolib.c src/lmathlib.c src/loadlib.c
        src/loslib.c src/lstrlib.c src/ltablib.c src/lutf8lib.c src/linit.c)

add_library(lua_static STATIC ${src_code} ${src_lib})
set_target_properties(lua_static PROPERTIES
    OUTPUT_NAME Lua
    POSITION_INDEPENDENT_CODE ON
)

if(UNIX)
    target_link_libraries(lua_static PRIVATE dl)
endif()

target_include_directories(lua_static PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

#add_library(liblua_share SHARED ${src_code} ${src_lib})
#set_target_properties(liblua_share PROPERTIES OUTPUT_NAME Lua)

#add_executable(lua ${src_code} ${src_lib} ${src_exec})
#set_target_properties(lua PROPERTIES OUTPUT_NAME lua)

#add_executable(luac ${src_code} ${src_lib} ${src_comp})
#set_target_properties(luac PROPERTIES OUTPUT_NAME luac)