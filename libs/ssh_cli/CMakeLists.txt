cmake_minimum_required(VERSION 3.12)
project(ssh_cli)

add_library(ssh_cli src/ssh_cli_client.cpp src/ssh_cli_session.cpp src/ssh_cli_shell_session.cpp src/ssh_cli_transport_stub.cpp src/ssh_cli_transport_loopback.cpp src/ssh_cli_transport_process.cpp)
target_link_libraries(ssh_cli PUBLIC SshProto)
target_include_directories(ssh_cli PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

add_executable(ssh_cli_demo src/main_ssh_cli_demo.cpp)
target_link_libraries(ssh_cli_demo PUBLIC ssh_cli)

if (YUAN_BUILD_TESTS)
    add_executable(test_ssh_cli_stub test/TestSshCliStub.cpp)
    target_link_libraries(test_ssh_cli_stub PUBLIC ssh_cli)

    add_executable(test_ssh_cli_loopback test/TestSshCliLoopback.cpp)
    target_link_libraries(test_ssh_cli_loopback PUBLIC ssh_cli)

    add_executable(test_ssh_cli_process_probe test/TestSshCliProcessProbe.cpp)
    target_link_libraries(test_ssh_cli_process_probe PUBLIC ssh_cli)

    add_executable(test_ssh_cli_process_local test/TestSshCliProcessLocal.cpp)
    target_link_libraries(test_ssh_cli_process_local PUBLIC ssh_cli)

    if (BUILD_TESTING)
        add_test(NAME ssh_cli_stub COMMAND test_ssh_cli_stub)
        add_test(NAME ssh_cli_loopback COMMAND test_ssh_cli_loopback)
        add_test(NAME ssh_cli_process_probe COMMAND test_ssh_cli_process_probe)
        add_test(NAME ssh_cli_process_local COMMAND test_ssh_cli_process_local)

        set_tests_properties(ssh_cli_stub PROPERTIES
            LABELS "ssh_cli;unit"
            TIMEOUT 60
        )
        set_tests_properties(ssh_cli_loopback PROPERTIES
            LABELS "ssh_cli;unit"
            TIMEOUT 60
        )
        set_tests_properties(ssh_cli_process_probe PROPERTIES
            LABELS "ssh_cli;integration;smoke"
            TIMEOUT 120
        )
        set_tests_properties(ssh_cli_process_local PROPERTIES
            LABELS "ssh_cli;integration;smoke"
            TIMEOUT 180
        )
    endif()
endif()
