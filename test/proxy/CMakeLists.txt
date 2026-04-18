option(YUAN_PROXY_TOOL_LINK_STATIC_RUNTIME "Link proxy_tool with static libstdc++/libgcc when supported" OFF)

add_executable(proxy_tool proxy_tool.cpp)
target_link_libraries(proxy_tool PUBLIC Socks5Proto)

if (YUAN_PROXY_TOOL_LINK_STATIC_RUNTIME AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(proxy_tool PRIVATE -static-libstdc++ -static-libgcc)
endif()

if (UNIX AND NOT APPLE)
    target_link_libraries(proxy_tool PRIVATE pthread)
endif()
