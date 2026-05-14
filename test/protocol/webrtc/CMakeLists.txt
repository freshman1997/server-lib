add_executable(test_webrtc_bridge test_webrtc_bridge.cpp)
target_link_libraries(test_webrtc_bridge PUBLIC WebrtcProto RtcpProto RtcProto)

add_executable(test_webrtc_sdp test_webrtc_sdp.cpp)
target_link_libraries(test_webrtc_sdp PUBLIC WebrtcProto)

add_executable(test_release_webrtc_server_smoke test_release_webrtc_server_smoke.cpp)
target_link_libraries(test_release_webrtc_server_smoke PUBLIC WebrtcProto)

if(BUILD_TESTING)
    add_test(NAME webrtc_bridge COMMAND test_webrtc_bridge)
    add_test(NAME webrtc_sdp COMMAND test_webrtc_sdp)
    add_test(NAME release_webrtc_server_smoke COMMAND test_release_webrtc_server_smoke)
endif()
