add_executable(test_rtc test_rtc.cpp)
target_link_libraries(test_rtc PUBLIC RtcProto)

add_executable(test_rtp_session test_rtp_session.cpp)
target_link_libraries(test_rtp_session PUBLIC RtcProto)

add_executable(test_rtp_session_manager test_rtp_session_manager.cpp)
target_link_libraries(test_rtp_session_manager PUBLIC RtcProto)

if(BUILD_TESTING)
    add_test(NAME rtc COMMAND test_rtc)
    add_test(NAME rtp_session COMMAND test_rtp_session)
    add_test(NAME rtp_session_manager COMMAND test_rtp_session_manager)
endif()
