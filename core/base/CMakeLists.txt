cmake_minimum_required(VERSION 3.12)
project(CoreBase)

file(GLOB_RECURSE CORE_BASE_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(CoreBase ${CORE_BASE_SRC})
target_compile_features(CoreBase PUBLIC cxx_std_20)
target_include_directories(CoreBase PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
