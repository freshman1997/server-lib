cmake_minimum_required(VERSION 3.12)
project(ServerProxy)

set(SERVER_PROXY_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy_library_anchor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy_service.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/server_runtime_host.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/reverse_proxy.cpp"
)

add_library(ServerProxy ${SERVER_PROXY_SRC})
target_compile_features(ServerProxy PUBLIC cxx_std_20)
target_link_libraries(ServerProxy PUBLIC App Core Logger HttpProto)
target_include_directories(ServerProxy PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/server/services/include"
    "${CMAKE_SOURCE_DIR}/protocol/socks5/include"
)
