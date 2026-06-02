cmake_minimum_required(VERSION 3.12)
project(release_proxy)

option(YUAN_PROXY_TOOL_LINK_STATIC_RUNTIME "Link release_proxy_tool with static libstdc++/libgcc when supported" ${YUAN_GLOBAL_STATIC_LINK})

set(RELEASE_PROXY_OUTPUT_DIR "${CMAKE_BINARY_DIR}/release/proxy")

add_executable(release_proxy_tool proxy_tool_main.cpp)
target_compile_features(release_proxy_tool PUBLIC cxx_std_20)
target_link_libraries(release_proxy_tool PUBLIC Socks5Proto)
target_include_directories(release_proxy_tool PUBLIC
    "${CMAKE_SOURCE_DIR}"
)
if (YUAN_PROXY_TOOL_LINK_STATIC_RUNTIME AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(release_proxy_tool PRIVATE -static-libstdc++ -static-libgcc)
endif()
if (UNIX AND NOT APPLE)
    target_link_libraries(release_proxy_tool PRIVATE pthread)
endif()
set_target_properties(release_proxy_tool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_PROXY_OUTPUT_DIR}"
)

add_executable(release_proxy_server proxy_server_main.cpp)
target_compile_features(release_proxy_server PUBLIC cxx_std_20)
target_link_libraries(release_proxy_server PUBLIC ServerProxy ServerServiceSocks5)
target_include_directories(release_proxy_server PUBLIC
    "${CMAKE_SOURCE_DIR}"
)
if (UNIX AND NOT APPLE)
    target_link_libraries(release_proxy_server PRIVATE pthread)
endif()
set_target_properties(release_proxy_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_PROXY_OUTPUT_DIR}"
)

add_custom_command(
    TARGET release_proxy_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/run_proxy_soak.ps1"
            "${RELEASE_PROXY_OUTPUT_DIR}/run_proxy_soak.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/run_proxy_stream_soak.ps1"
            "${RELEASE_PROXY_OUTPUT_DIR}/run_proxy_stream_soak.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/analyze_proxy_soak.ps1"
            "${RELEASE_PROXY_OUTPUT_DIR}/analyze_proxy_soak.ps1"
    COMMENT "Copy proxy soak scripts"
)
