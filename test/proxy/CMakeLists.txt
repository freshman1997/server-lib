add_executable(proxy_tool proxy_tool.cpp)
target_link_libraries(proxy_tool PUBLIC Socks5Proto)
