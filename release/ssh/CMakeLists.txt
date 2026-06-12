cmake_minimum_required(VERSION 3.12)
project(release_ssh)

set(RELEASE_SSH_OUTPUT_DIR "${CMAKE_BINARY_DIR}/release/ssh")

add_executable(release_ssh_server
    ssh_server_main.cpp
    server/ssh_server_config.cpp
    server/ssh_server_handler.cpp
)
target_compile_features(release_ssh_server PUBLIC cxx_std_20)
target_link_libraries(release_ssh_server PUBLIC Core SshProto)
target_include_directories(release_ssh_server PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/protocol/ssh/include"
)
if (YUAN_GLOBAL_STATIC_LINK AND UNIX AND NOT APPLE)
    target_link_options(release_ssh_server PRIVATE "-static-libgcc" "-static-libstdc++")
endif()
set_target_properties(release_ssh_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_SSH_OUTPUT_DIR}"
)

add_executable(release_ssh_cli
    ssh_cli_main.cpp
    client/ssh_cli_auth.cpp
    client/ssh_cli_config.cpp
    client/ssh_cli_forward.cpp
    client/ssh_cli_forward_runtime.cpp
    client/ssh_cli_messages.cpp
    client/ssh_cli_session.cpp
    client/ssh_cli_socket.cpp
    client/ssh_cli_terminal.cpp
    client/ssh_cli_transport.cpp
)
target_compile_features(release_ssh_cli PUBLIC cxx_std_20)
target_link_libraries(release_ssh_cli PUBLIC SshProto Core)
target_include_directories(release_ssh_cli PUBLIC "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/protocol/ssh/include")
if (YUAN_GLOBAL_STATIC_LINK AND UNIX AND NOT APPLE)
    target_link_options(release_ssh_cli PRIVATE "-static-libgcc" "-static-libstdc++")
endif()
set_target_properties(release_ssh_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_SSH_OUTPUT_DIR}"
)

if (WIN32)
    target_link_libraries(release_ssh_cli PUBLIC ws2_32)
endif()

add_custom_command(
    TARGET release_ssh_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
            "${RELEASE_SSH_OUTPUT_DIR}/config.json"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${RELEASE_SSH_OUTPUT_DIR}/hostkey"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/hostkey/ssh_host_ed25519_key"
            "${RELEASE_SSH_OUTPUT_DIR}/hostkey/ssh_host_ed25519_key"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.sh"
            "${RELEASE_SSH_OUTPUT_DIR}/health_check.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.sh"
            "${RELEASE_SSH_OUTPUT_DIR}/start.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.sh"
            "${RELEASE_SSH_OUTPUT_DIR}/stop.sh"
    COMMENT "Copy release ssh default config"
)
