cmake_minimum_required(VERSION 3.12)
project(release_rtsp)

set(RELEASE_RTSP_OUTPUT_DIR "${CMAKE_BINARY_DIR}/release/rtsp")

add_executable(release_rtsp_server
    rtsp_server_config.cpp
    rtsp_server_main.cpp
)
target_compile_features(release_rtsp_server PUBLIC cxx_std_20)
target_link_libraries(release_rtsp_server PUBLIC Core RtspProto)
target_include_directories(release_rtsp_server PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/protocol/rtsp/include"
)
if (UNIX AND NOT APPLE)
    target_link_options(release_rtsp_server PRIVATE "-static-libgcc" "-static-libstdc++")
endif()
set_target_properties(release_rtsp_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_RTSP_OUTPUT_DIR}"
)

if (WIN32)
    target_link_libraries(release_rtsp_server PUBLIC ws2_32)
endif()

add_custom_command(
    TARGET release_rtsp_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
            "${RELEASE_RTSP_OUTPUT_DIR}/config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${RELEASE_RTSP_OUTPUT_DIR}/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.sh"
            "${RELEASE_RTSP_OUTPUT_DIR}/health_check.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.sh"
            "${RELEASE_RTSP_OUTPUT_DIR}/start.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.sh"
            "${RELEASE_RTSP_OUTPUT_DIR}/stop.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.ps1"
            "${RELEASE_RTSP_OUTPUT_DIR}/start.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.ps1"
            "${RELEASE_RTSP_OUTPUT_DIR}/stop.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.ps1"
            "${RELEASE_RTSP_OUTPUT_DIR}/health_check.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
            "${RELEASE_RTSP_OUTPUT_DIR}/scripts"
    COMMENT "Copy release rtsp assets"
)
