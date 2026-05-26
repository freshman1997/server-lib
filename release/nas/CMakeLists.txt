cmake_minimum_required(VERSION 3.12)
project(release_nas)

set(RELEASE_NAS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/release/nas")
if (CMAKE_CONFIGURATION_TYPES)
    set(RELEASE_NAS_ASSET_DIR "${RELEASE_NAS_OUTPUT_DIR}/$<CONFIG>")
else()
    set(RELEASE_NAS_ASSET_DIR "${RELEASE_NAS_OUTPUT_DIR}")
endif()

add_executable(release_nas_server
    nas_server_main.cpp
)
target_compile_features(release_nas_server PUBLIC cxx_std_20)
target_link_libraries(release_nas_server PUBLIC App ServerServiceNas)
target_include_directories(release_nas_server PUBLIC
    "${CMAKE_SOURCE_DIR}"
)
if (UNIX AND NOT APPLE)
    target_link_options(release_nas_server PRIVATE "-static-libgcc" "-static-libstdc++")
endif()
set_target_properties(release_nas_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_NAS_OUTPUT_DIR}"
)

if (WIN32)
    target_link_libraries(release_nas_server PUBLIC ws2_32)
endif()

add_custom_target(release_nas_assets
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${RELEASE_NAS_ASSET_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
            "${RELEASE_NAS_ASSET_DIR}/config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${RELEASE_NAS_ASSET_DIR}/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/CLIENTS.md"
            "${RELEASE_NAS_ASSET_DIR}/CLIENTS.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/server/services/resources/nas_admin_console.html"
            "${RELEASE_NAS_ASSET_DIR}/nas_admin_console.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.sh"
            "${RELEASE_NAS_ASSET_DIR}/start.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.sh"
            "${RELEASE_NAS_ASSET_DIR}/stop.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.sh"
            "${RELEASE_NAS_ASSET_DIR}/health_check.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/gate.sh"
            "${RELEASE_NAS_ASSET_DIR}/gate.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.ps1"
            "${RELEASE_NAS_ASSET_DIR}/start.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.ps1"
            "${RELEASE_NAS_ASSET_DIR}/stop.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.ps1"
            "${RELEASE_NAS_ASSET_DIR}/health_check.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/gate.ps1"
            "${RELEASE_NAS_ASSET_DIR}/gate.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/generate_password_hash.ps1"
            "${RELEASE_NAS_ASSET_DIR}/generate_password_hash.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/init_config.ps1"
            "${RELEASE_NAS_ASSET_DIR}/init_config.ps1"
    COMMENT "Copy release nas assets"
)
add_dependencies(release_nas_server release_nas_assets)
