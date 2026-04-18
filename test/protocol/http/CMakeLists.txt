add_executable(http_server test_http_server.cpp)
add_executable(http_client test_http_client.cpp)

target_link_libraries(http_server PUBLIC HttpProto ServerServices)
target_link_libraries(http_client PUBLIC HttpProto)
