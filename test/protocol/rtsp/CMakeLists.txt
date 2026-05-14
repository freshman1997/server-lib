add_executable(test_rtsp test_rtsp.cpp)
target_link_libraries(test_rtsp PUBLIC RtspProto)

add_executable(test_rtsp_framing test_rtsp_framing.cpp)
target_link_libraries(test_rtsp_framing PUBLIC RtspProto)

add_executable(test_rtsp_session test_rtsp_session.cpp)
target_link_libraries(test_rtsp_session PUBLIC RtspProto)

add_executable(test_rtsp_play test_rtsp_play.cpp)
target_link_libraries(test_rtsp_play PUBLIC RtspProto)

add_executable(test_rtsp_server test_rtsp_server.cpp)
target_link_libraries(test_rtsp_server PUBLIC RtspProto)

add_executable(test_rtsp_state_matrix test_rtsp_state_matrix.cpp)
target_link_libraries(test_rtsp_state_matrix PUBLIC RtspProto)

add_executable(test_rtsp_interop test_rtsp_interop.cpp)
target_link_libraries(test_rtsp_interop PUBLIC RtspProto)

add_executable(test_release_rtsp_server_smoke test_release_rtsp_server_smoke.cpp)
target_link_libraries(test_release_rtsp_server_smoke PUBLIC RtspProto)

if(BUILD_TESTING)
    add_test(NAME rtsp COMMAND test_rtsp)
    add_test(NAME rtsp_framing COMMAND test_rtsp_framing)
    add_test(NAME rtsp_session COMMAND test_rtsp_session)
    add_test(NAME rtsp_play COMMAND test_rtsp_play)
    add_test(NAME rtsp_server COMMAND test_rtsp_server)
    add_test(NAME rtsp_state_matrix COMMAND test_rtsp_state_matrix)
    add_test(NAME rtsp_interop COMMAND test_rtsp_interop)
    add_test(NAME release_rtsp_server_smoke COMMAND test_release_rtsp_server_smoke)
endif()
