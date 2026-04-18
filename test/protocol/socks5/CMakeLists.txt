add_executable(test_socks5 test_socks5.cpp)
target_link_libraries(test_socks5 PUBLIC Socks5Proto)

if(BUILD_TESTING)
    add_test(NAME socks5 COMMAND test_socks5)
endif()
