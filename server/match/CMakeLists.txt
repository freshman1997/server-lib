cmake_minimum_required(VERSION 3.12)
project(match_server)

file(GLOB_RECURSE MATCH_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(match_server ${MATCH_SRC} main.cpp)
target_compile_features(match_server PUBLIC cxx_std_20)
target_link_libraries(match_server PUBLIC App Core Logger fmt::fmt)
target_include_directories(match_server PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

set_target_properties(match_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET match_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/config"
        "${CMAKE_BINARY_DIR}/bin/config"
)
