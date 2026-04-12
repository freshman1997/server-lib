cmake_minimum_required(VERSION 3.12)
project(App)

file(GLOB_RECURSE APP_ALL_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(App ${APP_ALL_SRC})
target_compile_features(App PUBLIC cxx_std_20)
target_link_libraries(App PUBLIC Core Logger redisCli)
target_include_directories(App PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
