option(YUAN_PROXY_TOOL_LINK_STATIC_RUNTIME "Link proxy_tool with static libstdc++/libgcc when supported" OFF)

add_executable(proxy_tool proxy_tool.cpp)
target_link_libraries(proxy_tool PUBLIC Socks5Proto)

add_executable(proxy_server proxy_server.cpp)
target_link_libraries(proxy_server PUBLIC ServerServices)

add_executable(test_proxy_service_integration test_proxy_service_integration.cpp)
target_link_libraries(test_proxy_service_integration PUBLIC ServerServices)

add_executable(test_server_runtime_host test_server_runtime_host.cpp)
target_link_libraries(test_server_runtime_host PUBLIC ServerServices)

if (YUAN_PROXY_TOOL_LINK_STATIC_RUNTIME AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(proxy_tool PRIVATE -static-libstdc++ -static-libgcc)
endif()

if (UNIX AND NOT APPLE)
    target_link_libraries(proxy_tool PRIVATE pthread)
    target_link_libraries(proxy_server PRIVATE pthread)
    target_link_libraries(test_proxy_service_integration PRIVATE pthread)
    target_link_libraries(test_server_runtime_host PRIVATE pthread)
endif()

if(BUILD_TESTING)
    add_test(NAME proxy_service_integration COMMAND test_proxy_service_integration)
    add_test(NAME server_runtime_host COMMAND test_server_runtime_host)
endif()
