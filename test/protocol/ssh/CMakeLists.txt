add_executable(test_ssh test_ssh.cpp)
add_executable(test_ssh_openssh_smoke test_ssh_openssh_smoke.cpp)

target_link_libraries(test_ssh PUBLIC SshProto)
target_link_libraries(test_ssh_openssh_smoke PUBLIC SshProto)

if(BUILD_TESTING)
    add_test(NAME ssh COMMAND test_ssh)
    add_test(NAME ssh_openssh_smoke COMMAND test_ssh_openssh_smoke)
endif()
