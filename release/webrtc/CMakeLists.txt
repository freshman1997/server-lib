cmake_minimum_required(VERSION 3.12)
project(release_webrtc)

set(RELEASE_WEBRTC_OUTPUT_DIR "${CMAKE_BINARY_DIR}/release/webrtc")

add_executable(release_webrtc_server
    webrtc_server_main.cpp
)
target_compile_features(release_webrtc_server PUBLIC cxx_std_20)
target_link_libraries(release_webrtc_server PUBLIC Core WebrtcProto)
target_include_directories(release_webrtc_server PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/protocol/webrtc/include"
)
if (YUAN_GLOBAL_STATIC_LINK AND UNIX AND NOT APPLE)
    target_link_options(release_webrtc_server PRIVATE "-static-libgcc" "-static-libstdc++")
endif()
set_target_properties(release_webrtc_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_WEBRTC_OUTPUT_DIR}"
)

if (WIN32)
    target_link_libraries(release_webrtc_server PUBLIC ws2_32)
endif()

add_custom_command(
    TARGET release_webrtc_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
            "${RELEASE_WEBRTC_OUTPUT_DIR}/config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${RELEASE_WEBRTC_OUTPUT_DIR}/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.sh"
            "${RELEASE_WEBRTC_OUTPUT_DIR}/health_check.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.sh"
            "${RELEASE_WEBRTC_OUTPUT_DIR}/start.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.sh"
            "${RELEASE_WEBRTC_OUTPUT_DIR}/stop.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.ps1"
            "${RELEASE_WEBRTC_OUTPUT_DIR}/start.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.ps1"
            "${RELEASE_WEBRTC_OUTPUT_DIR}/stop.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.ps1"
            "${RELEASE_WEBRTC_OUTPUT_DIR}/health_check.ps1"
    COMMENT "Copy release webrtc assets"
)
