cmake_minimum_required(VERSION 3.12)
project(release_mini_nginx)

set(RELEASE_MINI_NGINX_OUTPUT_DIR "${CMAKE_BINARY_DIR}/release/mini_nginx")

add_executable(mini_nginx
    main.cpp
    mini_nginx_app.cpp
    mini_nginx_cli.cpp
    mini_nginx_common.cpp
    mini_nginx_config.cpp
    mini_nginx_runtime.cpp
)
target_compile_features(mini_nginx PUBLIC cxx_std_20)
target_link_libraries(mini_nginx PUBLIC App ServerServiceHttp)
set_target_properties(mini_nginx PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RELEASE_MINI_NGINX_OUTPUT_DIR}"
)

add_custom_command(
    TARGET mini_nginx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/mini_nginx.json"
            "${RELEASE_MINI_NGINX_OUTPUT_DIR}/mini_nginx.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${RELEASE_MINI_NGINX_OUTPUT_DIR}/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/start.sh"
            "${RELEASE_MINI_NGINX_OUTPUT_DIR}/start.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/stop.sh"
            "${RELEASE_MINI_NGINX_OUTPUT_DIR}/stop.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/health_check.sh"
            "${RELEASE_MINI_NGINX_OUTPUT_DIR}/health_check.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/gate.sh"
            "${RELEASE_MINI_NGINX_OUTPUT_DIR}/gate.sh"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${RELEASE_MINI_NGINX_OUTPUT_DIR}/www/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/www/index.html"
            "${RELEASE_MINI_NGINX_OUTPUT_DIR}/www/index.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/www/404.html"
            "${RELEASE_MINI_NGINX_OUTPUT_DIR}/www/404.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/www/assets/readme.txt"
            "${RELEASE_MINI_NGINX_OUTPUT_DIR}/www/assets/readme.txt"
    COMMENT "Copy release mini_nginx assets"
)
