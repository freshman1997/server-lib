cmake_minimum_required(VERSION 3.12)
project(GameBase)

add_library(GameBase INTERFACE)
target_compile_features(GameBase INTERFACE cxx_std_20)
target_include_directories(GameBase INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

target_link_libraries(GameBase INTERFACE YuanRpc GameCoroutine)

if(YUAN_BUILD_TESTS OR BUILD_TESTING)
    add_executable(test_game_base_kernel test/test_game_base_kernel.cpp)
    add_executable(test_game_base_algorithms test/test_game_base_algorithms.cpp)
    add_executable(test_game_base_world_map test/test_game_base_world_map.cpp)
    target_link_libraries(test_game_base_kernel PUBLIC GameBase)
    target_link_libraries(test_game_base_algorithms PUBLIC GameBase)
    target_link_libraries(test_game_base_world_map PUBLIC GameBase)
    if(BUILD_TESTING)
        add_test(NAME game_base_kernel COMMAND test_game_base_kernel)
        add_test(NAME game_base_algorithms COMMAND test_game_base_algorithms)
        add_test(NAME game_base_world_map COMMAND test_game_base_world_map)
    endif()
endif()
