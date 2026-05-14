add_executable(test_rtcp test_rtcp.cpp)
target_link_libraries(test_rtcp PUBLIC RtcpProto)

add_executable(test_rtcp_session test_rtcp_session.cpp)
target_link_libraries(test_rtcp_session PUBLIC RtcpProto RtcProto)

add_executable(test_rtcp_loopback test_rtcp_loopback.cpp)
target_link_libraries(test_rtcp_loopback PUBLIC RtcpProto RtcProto)

if(BUILD_TESTING)
    add_test(NAME rtcp COMMAND test_rtcp)
    add_test(NAME rtcp_session COMMAND test_rtcp_session)
    add_test(NAME rtcp_loopback COMMAND test_rtcp_loopback)
endif()
