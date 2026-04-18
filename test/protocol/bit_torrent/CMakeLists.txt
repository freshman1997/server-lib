add_executable(test_bit_torrent test_bit_torrent.cpp)
add_executable(test_udp_tracker_coroutine test_udp_tracker_coroutine.cpp)

target_link_libraries(test_bit_torrent PUBLIC BitTorrentProto)
target_link_libraries(test_udp_tracker_coroutine PUBLIC BitTorrentProto)

if(BUILD_TESTING)
    add_test(NAME udp_tracker_coroutine COMMAND $<TARGET_FILE:test_udp_tracker_coroutine>)
endif()
