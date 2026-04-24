add_executable(http_server test_http_server.cpp)
add_executable(http_client test_http_client.cpp)
add_executable(test_http_features test_http_features.cpp)

target_link_libraries(http_server PUBLIC HttpProto ServerServices)
target_link_libraries(http_client PUBLIC HttpProto)
target_link_libraries(test_http_features PUBLIC HttpProto ServerServices)

if (BUILD_TESTING)
    add_test(NAME http_features COMMAND test_http_features)
endif()
