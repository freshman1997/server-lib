add_executable(test_ssh test_ssh.cpp)
add_executable(test_ssh_packet_codec_fuzz test_ssh_packet_codec_fuzz.cpp)
add_executable(test_ssh_openssh_smoke test_ssh_openssh_smoke.cpp)
add_executable(test_release_ssh_cli_smoke test_release_ssh_cli_smoke.cpp)
add_executable(test_release_ssh_cli_interactive_smoke test_release_ssh_cli_interactive_smoke.cpp)
add_executable(test_release_ssh_cli_interactive_stress_smoke test_release_ssh_cli_interactive_stress_smoke.cpp)
add_executable(test_release_ssh_cli_remote_forward_smoke test_release_ssh_cli_remote_forward_smoke.cpp)
add_executable(test_release_ssh_cli_local_forward_smoke test_release_ssh_cli_local_forward_smoke.cpp)
add_executable(test_release_ssh_cli_dynamic_forward_smoke test_release_ssh_cli_dynamic_forward_smoke.cpp)
add_executable(test_release_ssh_cli_mixed_forward_soak test_release_ssh_cli_mixed_forward_soak.cpp)

target_link_libraries(test_ssh PUBLIC SshProto)
target_link_libraries(test_ssh_packet_codec_fuzz PUBLIC SshProto)
target_link_libraries(test_ssh_openssh_smoke PUBLIC SshProto)
target_link_libraries(test_release_ssh_cli_smoke PUBLIC SshProto)
target_link_libraries(test_release_ssh_cli_interactive_smoke PUBLIC SshProto)
target_link_libraries(test_release_ssh_cli_interactive_stress_smoke PUBLIC SshProto)
target_link_libraries(test_release_ssh_cli_remote_forward_smoke PUBLIC SshProto)
target_link_libraries(test_release_ssh_cli_local_forward_smoke PUBLIC SshProto)
target_link_libraries(test_release_ssh_cli_dynamic_forward_smoke PUBLIC SshProto)
target_link_libraries(test_release_ssh_cli_mixed_forward_soak PUBLIC SshProto)

if(BUILD_TESTING)
    add_test(NAME ssh COMMAND test_ssh)
    add_test(NAME ssh_packet_codec_fuzz COMMAND test_ssh_packet_codec_fuzz)
    add_test(NAME ssh_openssh_smoke COMMAND test_ssh_openssh_smoke)
    add_test(NAME ssh_release_cli_smoke COMMAND test_release_ssh_cli_smoke)
    add_test(NAME ssh_release_cli_interactive_smoke COMMAND test_release_ssh_cli_interactive_smoke)
    add_test(NAME ssh_release_cli_interactive_stress_smoke COMMAND test_release_ssh_cli_interactive_stress_smoke)
    add_test(NAME ssh_release_cli_remote_forward_smoke COMMAND test_release_ssh_cli_remote_forward_smoke)
    add_test(NAME ssh_release_cli_local_forward_smoke COMMAND test_release_ssh_cli_local_forward_smoke)
    add_test(NAME ssh_release_cli_dynamic_forward_smoke COMMAND test_release_ssh_cli_dynamic_forward_smoke)
    add_test(NAME ssh_release_cli_mixed_forward_soak COMMAND test_release_ssh_cli_mixed_forward_soak)

    set_tests_properties(ssh PROPERTIES
        LABELS "ssh;unit"
        TIMEOUT 90
    )

    set_tests_properties(ssh_packet_codec_fuzz PROPERTIES
        LABELS "ssh;fuzz;hardening"
        TIMEOUT 120
    )

    set_tests_properties(ssh_openssh_smoke PROPERTIES
        LABELS "ssh;integration;smoke"
        TIMEOUT 180
    )

    set_tests_properties(ssh_release_cli_smoke PROPERTIES
        LABELS "ssh;integration;smoke"
        TIMEOUT 180
    )

    set_tests_properties(ssh_release_cli_interactive_smoke PROPERTIES
        LABELS "ssh;integration;smoke"
        TIMEOUT 180
    )

    set_tests_properties(ssh_release_cli_interactive_stress_smoke PROPERTIES
        LABELS "ssh;integration;smoke"
        TIMEOUT 180
    )

    set_tests_properties(ssh_release_cli_remote_forward_smoke PROPERTIES
        LABELS "ssh;integration;smoke"
        TIMEOUT 180
    )

    set_tests_properties(ssh_release_cli_local_forward_smoke PROPERTIES
        LABELS "ssh;integration;smoke"
        TIMEOUT 180
    )

    set_tests_properties(ssh_release_cli_dynamic_forward_smoke PROPERTIES
        LABELS "ssh;integration;smoke"
        TIMEOUT 180
    )

    set_tests_properties(ssh_release_cli_mixed_forward_soak PROPERTIES
        LABELS "ssh;integration;soak;nightly"
        TIMEOUT 240
    )
endif()
