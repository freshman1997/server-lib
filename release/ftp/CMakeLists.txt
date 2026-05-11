cmake_minimum_required(VERSION 3.12)
project(release_ftp)

set(RELEASE_FTP_OUTPUT_DIR "${CMAKE_BINARY_DIR}/release/ftp")

add_executable(release_ftp_server ftp_server_main.cpp)
target_compile_features(release_ftp_server PUBLIC cxx_std_20)
target_link_libraries(release_ftp_server PUBLIC Core FtpProto)
target_include_directories(release_ftp_server PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/protocol/ftp/include"
)
if (UNIX AND NOT APPLE)
    target_link_options(release_ftp_server PRIVATE "-static-libgcc" "-static-libstdc++")
endif()
set_target_properties(release_ftp_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_FTP_OUTPUT_DIR}"
)

add_executable(release_ftp_cli ftp_cli_main.cpp)
target_compile_features(release_ftp_cli PUBLIC cxx_std_20)
target_link_libraries(release_ftp_cli PUBLIC Core FtpProto)
target_include_directories(release_ftp_cli PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/protocol/ftp/include"
)
if (UNIX AND NOT APPLE)
    target_link_options(release_ftp_cli PRIVATE "-static-libgcc" "-static-libstdc++")
endif()
set_target_properties(release_ftp_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_FTP_OUTPUT_DIR}"
)

if (WIN32)
    target_link_libraries(release_ftp_cli PUBLIC ws2_32)
    target_link_libraries(release_ftp_server PUBLIC ws2_32)
endif()

add_custom_command(
    TARGET release_ftp_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
            "${RELEASE_FTP_OUTPUT_DIR}/config.json"
    COMMENT "Copy release ftp default config"
)
