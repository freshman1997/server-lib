cmake_minimum_required(VERSION 3.12)
project(bt_downloader)

add_executable(bt_downloader main.cpp)
target_link_libraries(bt_downloader PUBLIC App ServerServices HttpProto)

set(BT_DOWNLOADER_WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(BT_DOWNLOADER_WEB_DST_DIR "${CMAKE_CURRENT_BINARY_DIR}/web")

add_custom_command(
    TARGET bt_downloader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${BT_DOWNLOADER_WEB_DST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${BT_DOWNLOADER_WEB_SRC_DIR}" "${BT_DOWNLOADER_WEB_DST_DIR}"
    COMMENT "Copy bt_downloader web assets to build output"
)

add_custom_command(
    TARGET bt_downloader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
            "${CMAKE_CURRENT_BINARY_DIR}/config.json"
    COMMENT "Copy bt_downloader default config"
)
