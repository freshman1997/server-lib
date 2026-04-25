cmake_minimum_required(VERSION 3.12)
project(NasCore)

set(NAS_CORE_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nas_auth_service.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nas_http_middleware.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nas_redis_webdav_lock_manager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nas_share_manager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nas_webdav_adapter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nas_webdav_backend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nas_webdav_mount.cpp"
)

if (TARGET redisCli)
    list(APPEND NAS_CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/nas_redis_metadata_store.cpp")
endif()

add_library(NasCore STATIC ${NAS_CORE_SRC})
target_compile_features(NasCore PUBLIC cxx_std_20)
target_link_libraries(NasCore PUBLIC CoreBase HttpProto WebDavProto)
if (TARGET redisCli)
    target_link_libraries(NasCore PUBLIC redisCli)
    target_compile_definitions(NasCore PUBLIC YUAN_NAS_HAS_REDIS=1)
else()
    target_compile_definitions(NasCore PUBLIC YUAN_NAS_HAS_REDIS=0)
endif()
target_include_directories(NasCore
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
