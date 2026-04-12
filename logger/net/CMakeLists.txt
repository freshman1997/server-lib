cmake_minimum_required(VERSION 3.12)
project(NetLogger)

# 网络日志扩展库：依赖 Logger 基础库和 Core 网络模块。
file(GLOB_RECURSE NET_SRC "*.cpp" "*.h")

add_library(${PROJECT_NAME} ${NET_SRC})

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)

target_link_libraries(${PROJECT_NAME}
    PUBLIC
        Logger
        Core
        fmt
)

target_include_directories(${PROJECT_NAME}
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
