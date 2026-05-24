cmake_minimum_required(VERSION 3.12)
project(release_bt_downloader)

set(RELEASE_BT_DOWNLOADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/release/bt_downloader")

add_executable(bt_downloader main.cpp)
target_compile_features(bt_downloader PUBLIC cxx_std_20)
target_link_libraries(bt_downloader PUBLIC App ServerServiceBitTorrent ServerServiceHttp HttpProto)
set_target_properties(bt_downloader PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_BT_DOWNLOADER_OUTPUT_DIR}"
)

set(BT_DOWNLOADER_WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(BT_DOWNLOADER_WEB_DST_DIR "${RELEASE_BT_DOWNLOADER_OUTPUT_DIR}/web")

add_custom_command(
    TARGET bt_downloader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${BT_DOWNLOADER_WEB_DST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${BT_DOWNLOADER_WEB_SRC_DIR}" "${BT_DOWNLOADER_WEB_DST_DIR}"
    COMMENT "Copy bt_downloader web assets to build output"
)

add_custom_command(
    TARGET bt_downloader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
            "${RELEASE_BT_DOWNLOADER_OUTPUT_DIR}/config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${RELEASE_BT_DOWNLOADER_OUTPUT_DIR}/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.sh"
            "${RELEASE_BT_DOWNLOADER_OUTPUT_DIR}/start.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.sh"
            "${RELEASE_BT_DOWNLOADER_OUTPUT_DIR}/stop.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.sh"
            "${RELEASE_BT_DOWNLOADER_OUTPUT_DIR}/health_check.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/gate.sh"
            "${RELEASE_BT_DOWNLOADER_OUTPUT_DIR}/gate.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.ps1"
            "${RELEASE_BT_DOWNLOADER_OUTPUT_DIR}/start.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.ps1"
            "${RELEASE_BT_DOWNLOADER_OUTPUT_DIR}/stop.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.ps1"
            "${RELEASE_BT_DOWNLOADER_OUTPUT_DIR}/health_check.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/gate.ps1"
            "${RELEASE_BT_DOWNLOADER_OUTPUT_DIR}/gate.ps1"
    COMMENT "Copy bt_downloader default config"
)
