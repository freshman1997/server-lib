add_executable(game_mock_client mock_client.cpp)
target_compile_features(game_mock_client PUBLIC cxx_std_20)
target_include_directories(game_mock_client PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>")
target_link_libraries(game_mock_client PUBLIC GameServerCommon HttpProto)
