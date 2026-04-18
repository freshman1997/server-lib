add_executable(test_kcp_cli test_udp_kcp_cli.cpp)
add_executable(test_kcp_svr test_udp_kcp_svr.cpp)

target_link_libraries(test_kcp_cli PUBLIC DnsProto)
target_link_libraries(test_kcp_svr PUBLIC DnsProto)
