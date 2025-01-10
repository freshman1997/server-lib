cmake_minimum_required(VERSION 3.12)
project(App)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static -fPIC")

FILE(GLOB_RECURSE APP_SRC "./src/*.cpp")

add_library(App ${APP_SRC})

target_link_libraries(App PUBLIC Core Logger)

target_include_directories(App PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
