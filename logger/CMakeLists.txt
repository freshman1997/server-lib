cmake_minimum_required(VERSION 3.12)
project(Logger)

FILE(GLOB_RECURSE LOG_SRC "./src/*.cpp")

add_library(${PROJECT_NAME} ${LOG_SRC})

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)

target_link_libraries(${PROJECT_NAME}
    PUBLIC
        CoreBase
        fmt
        Threads::Threads
)

target_include_directories(${PROJECT_NAME}
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

# ============================================================
# NetLogger 独立子库（依赖 Logger + Core，可选链接）
# 解决 Core ↔ Logger 循环依赖问题
# ============================================================
add_subdirectory(net)
