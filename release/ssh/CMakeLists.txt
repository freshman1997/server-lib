cmake_minimum_required(VERSION 3.12)
project(release_ssh)

set(RELEASE_SSH_OUTPUT_DIR "${CMAKE_BINARY_DIR}/release/ssh")

add_executable(release_ssh_server ssh_server_main.cpp)
target_compile_features(release_ssh_server PUBLIC cxx_std_20)
target_link_libraries(release_ssh_server PUBLIC Core SshProto)
target_include_directories(release_ssh_server PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/protocol/ssh/include"
)
if (UNIX AND NOT APPLE)
    target_link_options(release_ssh_server PRIVATE "-static-libgcc" "-static-libstdc++")
endif()
set_target_properties(release_ssh_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_SSH_OUTPUT_DIR}"
)

add_executable(release_ssh_cli ssh_cli_main.cpp)
target_compile_features(release_ssh_cli PUBLIC cxx_std_20)
target_link_libraries(release_ssh_cli PUBLIC SshProto Core)
target_include_directories(release_ssh_cli PUBLIC "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/protocol/ssh/include")
if (UNIX AND NOT APPLE)
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
    COMMENT "Copy release ssh default config"
)
