add_executable(test_shadowsocks test_shadowsocks.cpp)
target_link_libraries(test_shadowsocks PUBLIC ShadowsocksProto)

add_executable(shadowsocks_server_tool shadowsocks_server_tool.cpp)
target_link_libraries(shadowsocks_server_tool PUBLIC ShadowsocksProto)

if(BUILD_TESTING)
    add_test(NAME shadowsocks COMMAND test_shadowsocks)
endif()
