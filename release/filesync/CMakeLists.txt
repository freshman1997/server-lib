cmake_minimum_required(VERSION 3.12)
project(release_filesync)

find_package(Threads REQUIRED)

set(RELEASE_FILESYNC_OUTPUT_DIR "${CMAKE_BINARY_DIR}/release/filesync")

add_executable(release_filesync_server filesync_peer.cpp)
target_compile_features(release_filesync_server PUBLIC cxx_std_20)
target_compile_definitions(release_filesync_server PRIVATE FILESYNC_DEFAULT_CONFIG="server_config.json")
target_include_directories(release_filesync_server PUBLIC
    "${CMAKE_SOURCE_DIR}/third_party/json/include"
    "${CMAKE_SOURCE_DIR}/core/core/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(release_filesync_server PUBLIC Core)
if (WIN32)
    target_link_libraries(release_filesync_server PUBLIC ws2_32)
else()
    target_link_libraries(release_filesync_server PUBLIC Threads::Threads)
endif()
if (YUAN_GLOBAL_STATIC_LINK AND UNIX AND NOT APPLE)
    target_link_options(release_filesync_server PRIVATE "-static" "-static-libgcc" "-static-libstdc++")
endif()
set_target_properties(release_filesync_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_FILESYNC_OUTPUT_DIR}"
)

add_executable(release_filesync_client filesync_peer.cpp)
target_compile_features(release_filesync_client PUBLIC cxx_std_20)
target_compile_definitions(release_filesync_client PRIVATE FILESYNC_DEFAULT_CONFIG="client_config.json")
target_include_directories(release_filesync_client PUBLIC
    "${CMAKE_SOURCE_DIR}/third_party/json/include"
    "${CMAKE_SOURCE_DIR}/core/core/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(release_filesync_client PUBLIC Core)
if (WIN32)
    target_link_libraries(release_filesync_client PUBLIC ws2_32)
else()
    target_link_libraries(release_filesync_client PUBLIC Threads::Threads)
endif()
if (YUAN_GLOBAL_STATIC_LINK AND UNIX AND NOT APPLE)
    target_link_options(release_filesync_client PRIVATE "-static" "-static-libgcc" "-static-libstdc++")
endif()
set_target_properties(release_filesync_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_FILESYNC_OUTPUT_DIR}"
)

add_executable(release_filesync_tests filesync_tests.cpp)
target_compile_features(release_filesync_tests PUBLIC cxx_std_20)
target_include_directories(release_filesync_tests PUBLIC
    "${CMAKE_SOURCE_DIR}/third_party/json/include"
    "${CMAKE_SOURCE_DIR}/core/core/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(release_filesync_tests PUBLIC Core)
if (WIN32)
    target_link_libraries(release_filesync_tests PUBLIC ws2_32)
else()
    target_link_libraries(release_filesync_tests PUBLIC Threads::Threads)
endif()
set_target_properties(release_filesync_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_FILESYNC_OUTPUT_DIR}"
)

if(BUILD_TESTING)
    add_test(NAME release_filesync_unit COMMAND release_filesync_tests)
    if(WIN32)
        add_test(
            NAME release_filesync_many_tree_e2e
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/filesync_many_tree_e2e.ps1"
                    -ServerExe $<TARGET_FILE:release_filesync_server>
                    -ClientExe $<TARGET_FILE:release_filesync_client>
        )
        set_tests_properties(release_filesync_many_tree_e2e PROPERTIES TIMEOUT 150)
    endif()
endif()

add_custom_command(
    TARGET release_filesync_client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/client_config.json"
            "${RELEASE_FILESYNC_OUTPUT_DIR}/client_config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/server_config.json"
            "${RELEASE_FILESYNC_OUTPUT_DIR}/server_config.json"
    COMMENT "Copy release filesync default configs"
)

add_custom_command(
    TARGET release_filesync_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/client_config.json"
            "${RELEASE_FILESYNC_OUTPUT_DIR}/client_config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/server_config.json"
            "${RELEASE_FILESYNC_OUTPUT_DIR}/server_config.json"
    COMMENT "Copy release filesync default configs"
)
