add_executable(test_proxy_service_integration test_proxy_service_integration.cpp)
target_link_libraries(test_proxy_service_integration PUBLIC ServerProxy)

add_executable(test_server_runtime_host test_server_runtime_host.cpp)
target_link_libraries(test_server_runtime_host PUBLIC ServerProxy)

if (UNIX AND NOT APPLE)
    target_link_libraries(test_proxy_service_integration PRIVATE pthread)
    target_link_libraries(test_server_runtime_host PRIVATE pthread)
endif()

if(BUILD_TESTING)
    add_test(NAME proxy_service_integration COMMAND test_proxy_service_integration)
    add_test(NAME server_runtime_host COMMAND test_server_runtime_host)
endif()
