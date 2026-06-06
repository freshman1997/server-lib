add_executable(ws_svr test_websocket_server.cpp)
add_executable(ws_cli test_websocket_client.cpp)
add_executable(test_websocket_protocol test_websocket_protocol.cpp)

target_link_libraries(ws_svr PUBLIC WebSocketProto ServerServiceWebSocket)
target_link_libraries(ws_cli PUBLIC WebSocketProto)
target_link_libraries(test_websocket_protocol PUBLIC WebSocketProto)

if(BUILD_TESTING)
    add_test(NAME websocket_protocol COMMAND test_websocket_protocol)
endif()
