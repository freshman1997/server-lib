cmake_minimum_required(VERSION 3.12)
project(release_dns)

set(RELEASE_DNS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/release/dns")
set(RELEASE_DNS_PACKAGE_ROOT "${CMAKE_BINARY_DIR}/release/packages")
set(RELEASE_DNS_PACKAGE_STAGE "${RELEASE_DNS_PACKAGE_ROOT}/release_dns")
set(RELEASE_DNS_VERSION "1.0.0")

if (WIN32)
    set(RELEASE_DNS_PLATFORM "windows")
elseif(APPLE)
    set(RELEASE_DNS_PLATFORM "macos")
else()
    set(RELEASE_DNS_PLATFORM "linux")
endif()

set(RELEASE_DNS_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if (NOT RELEASE_DNS_ARCH)
    set(RELEASE_DNS_ARCH "unknown")
endif()

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE RELEASE_DNS_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if (NOT RELEASE_DNS_GIT_COMMIT)
    set(RELEASE_DNS_GIT_COMMIT "unknown")
endif()

string(TIMESTAMP RELEASE_DNS_BUILD_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
set(RELEASE_DNS_PACKAGE_NAME "release_dns_${RELEASE_DNS_VERSION}_${RELEASE_DNS_PLATFORM}_${RELEASE_DNS_ARCH}.zip")
set(RELEASE_DNS_MANIFEST_FILE "${CMAKE_CURRENT_BINARY_DIR}/manifest.json")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/manifest.json.in" "${RELEASE_DNS_MANIFEST_FILE}" @ONLY)

add_executable(release_dns_server
    dns_server_main.cpp
)
target_compile_features(release_dns_server PUBLIC cxx_std_20)
target_link_libraries(release_dns_server PUBLIC Core DnsProto)
target_include_directories(release_dns_server PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/protocol/dns/include"
)
if (UNIX AND NOT APPLE)
    target_link_options(release_dns_server PRIVATE "-static-libgcc" "-static-libstdc++")
endif()
set_target_properties(release_dns_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_DNS_OUTPUT_DIR}"
)

if (WIN32)
    target_link_libraries(release_dns_server PUBLIC ws2_32)
endif()

add_custom_command(
    TARGET release_dns_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
            "${RELEASE_DNS_OUTPUT_DIR}/config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${RELEASE_DNS_OUTPUT_DIR}/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.sh"
            "${RELEASE_DNS_OUTPUT_DIR}/start.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.sh"
            "${RELEASE_DNS_OUTPUT_DIR}/stop.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.sh"
            "${RELEASE_DNS_OUTPUT_DIR}/health_check.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.ps1"
            "${RELEASE_DNS_OUTPUT_DIR}/start.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.ps1"
            "${RELEASE_DNS_OUTPUT_DIR}/stop.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.ps1"
            "${RELEASE_DNS_OUTPUT_DIR}/health_check.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RELEASE_DNS_MANIFEST_FILE}"
            "${RELEASE_DNS_OUTPUT_DIR}/manifest.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/gate.sh"
            "${RELEASE_DNS_OUTPUT_DIR}/gate.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/gate.ps1"
            "${RELEASE_DNS_OUTPUT_DIR}/gate.ps1"
    COMMENT "Copy release dns assets"
)

add_custom_target(release_dns_package
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_DNS_PACKAGE_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${RELEASE_DNS_PACKAGE_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_DNS_PACKAGE_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:release_dns_server>"
            "${RELEASE_DNS_PACKAGE_STAGE}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
            "${RELEASE_DNS_PACKAGE_STAGE}/config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${RELEASE_DNS_PACKAGE_STAGE}/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.sh"
            "${RELEASE_DNS_PACKAGE_STAGE}/start.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.sh"
            "${RELEASE_DNS_PACKAGE_STAGE}/stop.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.sh"
            "${RELEASE_DNS_PACKAGE_STAGE}/health_check.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.ps1"
            "${RELEASE_DNS_PACKAGE_STAGE}/start.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.ps1"
            "${RELEASE_DNS_PACKAGE_STAGE}/stop.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.ps1"
            "${RELEASE_DNS_PACKAGE_STAGE}/health_check.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RELEASE_DNS_MANIFEST_FILE}"
            "${RELEASE_DNS_PACKAGE_STAGE}/manifest.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/gate.sh"
            "${RELEASE_DNS_PACKAGE_STAGE}/gate.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/gate.ps1"
            "${RELEASE_DNS_PACKAGE_STAGE}/gate.ps1"
    COMMAND ${CMAKE_COMMAND} -E chdir "${RELEASE_DNS_PACKAGE_ROOT}"
            ${CMAKE_COMMAND} -E tar "cfv"
            "${RELEASE_DNS_PACKAGE_ROOT}/release_dns.zip"
            --format=zip
            "release_dns"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RELEASE_DNS_PACKAGE_ROOT}/release_dns.zip"
            "${RELEASE_DNS_PACKAGE_ROOT}/${RELEASE_DNS_PACKAGE_NAME}"
    DEPENDS release_dns_server
    COMMENT "Create DNS release zip package"
)
