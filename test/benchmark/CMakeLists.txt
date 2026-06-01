add_executable(core_runtime_benchmark core_runtime_benchmark.cpp)
target_link_libraries(core_runtime_benchmark PUBLIC Core App)

add_executable(http_worker_pool_benchmark http_worker_pool_benchmark.cpp)
target_link_libraries(http_worker_pool_benchmark PUBLIC HttpProto ServerServiceHttp)

add_executable(game_benchmark game_benchmark.cpp)
target_link_libraries(game_benchmark PUBLIC Core App)
if (WIN32)
    target_link_libraries(game_benchmark PRIVATE ws2_32)
endif()

set(YUAN_NETWORK_REPOS_ROOT "/home/yuan/codes/repos/network" CACHE PATH "Local network library checkout root for optional benchmark comparisons")

set(YUAN_LIBEVENT_ROOT "${YUAN_NETWORK_REPOS_ROOT}/libevent")
if (EXISTS "${YUAN_LIBEVENT_ROOT}/build-release/lib/libevent_extra.a"
    AND EXISTS "${YUAN_LIBEVENT_ROOT}/build-release/lib/libevent_core.a"
    AND EXISTS "${YUAN_LIBEVENT_ROOT}/build-release/lib/libevent_pthreads.a")
    set(YUAN_LIBEVENT_EXTRA "${YUAN_LIBEVENT_ROOT}/build-release/lib/libevent_extra.a")
    set(YUAN_LIBEVENT_CORE "${YUAN_LIBEVENT_ROOT}/build-release/lib/libevent_core.a")
    set(YUAN_LIBEVENT_PTHREADS "${YUAN_LIBEVENT_ROOT}/build-release/lib/libevent_pthreads.a")
    set(YUAN_LIBEVENT_GENERATED_INCLUDE "${YUAN_LIBEVENT_ROOT}/build-release/include")
elseif (EXISTS "${YUAN_LIBEVENT_ROOT}/build/lib/libevent_extrad.a"
    AND EXISTS "${YUAN_LIBEVENT_ROOT}/build/lib/libevent_cored.a"
    AND EXISTS "${YUAN_LIBEVENT_ROOT}/build/lib/libevent_pthreadsd.a")
    set(YUAN_LIBEVENT_EXTRA "${YUAN_LIBEVENT_ROOT}/build/lib/libevent_extrad.a")
    set(YUAN_LIBEVENT_CORE "${YUAN_LIBEVENT_ROOT}/build/lib/libevent_cored.a")
    set(YUAN_LIBEVENT_PTHREADS "${YUAN_LIBEVENT_ROOT}/build/lib/libevent_pthreadsd.a")
    set(YUAN_LIBEVENT_GENERATED_INCLUDE "${YUAN_LIBEVENT_ROOT}/build/include")
elseif (EXISTS "${YUAN_NETWORK_REPOS_ROOT}/libevent-2.1.8-stable/build-mingw-release/libevent_extra.a"
    AND EXISTS "${YUAN_NETWORK_REPOS_ROOT}/libevent-2.1.8-stable/build-mingw-release/libevent_core.a"
    AND EXISTS "${YUAN_NETWORK_REPOS_ROOT}/libevent-2.1.8-stable/build-mingw-release/libevent.a")
    set(YUAN_LIBEVENT_ROOT "${YUAN_NETWORK_REPOS_ROOT}/libevent-2.1.8-stable")
    set(YUAN_LIBEVENT_EXTRA "${YUAN_LIBEVENT_ROOT}/build-mingw-release/libevent_extra.a")
    set(YUAN_LIBEVENT_CORE "${YUAN_LIBEVENT_ROOT}/build-mingw-release/libevent_core.a")
    set(YUAN_LIBEVENT_PTHREADS "")
    set(YUAN_LIBEVENT_GENERATED_INCLUDE "${YUAN_LIBEVENT_ROOT}/WIN32-Code/nmake")
elseif (MSVC
    AND EXISTS "${YUAN_NETWORK_REPOS_ROOT}/libevent-2.1.8-stable/lib/libevent_extras.lib"
    AND EXISTS "${YUAN_NETWORK_REPOS_ROOT}/libevent-2.1.8-stable/lib/libevent_core.lib"
    AND EXISTS "${YUAN_NETWORK_REPOS_ROOT}/libevent-2.1.8-stable/lib/libevent.lib")
    set(YUAN_LIBEVENT_ROOT "${YUAN_NETWORK_REPOS_ROOT}/libevent-2.1.8-stable")
    set(YUAN_LIBEVENT_EXTRA "${YUAN_LIBEVENT_ROOT}/lib/libevent_extras.lib")
    set(YUAN_LIBEVENT_CORE "${YUAN_LIBEVENT_ROOT}/lib/libevent_core.lib")
    set(YUAN_LIBEVENT_PTHREADS "")
    set(YUAN_LIBEVENT_GENERATED_INCLUDE "${YUAN_LIBEVENT_ROOT}/WIN32-Code/nmake")
endif()
if (YUAN_LIBEVENT_EXTRA AND YUAN_LIBEVENT_CORE)
    target_compile_definitions(http_worker_pool_benchmark PRIVATE YUAN_BENCH_WITH_LIBEVENT=1)
    target_include_directories(http_worker_pool_benchmark PRIVATE
        "${YUAN_LIBEVENT_ROOT}/include"
        "${YUAN_LIBEVENT_GENERATED_INCLUDE}")
    target_link_libraries(http_worker_pool_benchmark PRIVATE
        "${YUAN_LIBEVENT_EXTRA}"
        "${YUAN_LIBEVENT_PTHREADS}"
        "${YUAN_LIBEVENT_CORE}")
    if (WIN32)
        target_link_libraries(http_worker_pool_benchmark PRIVATE ws2_32 iphlpapi)
    endif()
endif()

set(YUAN_LIBUV_ROOT "${YUAN_NETWORK_REPOS_ROOT}/libuv")
if (EXISTS "${YUAN_LIBUV_ROOT}/build-release/libuv.a")
    set(YUAN_LIBUV_LIBRARY "${YUAN_LIBUV_ROOT}/build-release/libuv.a")
elseif (EXISTS "${YUAN_LIBUV_ROOT}/build/libuv.a")
    set(YUAN_LIBUV_LIBRARY "${YUAN_LIBUV_ROOT}/build/libuv.a")
endif()
if (YUAN_LIBUV_LIBRARY)
    target_compile_definitions(http_worker_pool_benchmark PRIVATE YUAN_BENCH_WITH_LIBUV=1)
    target_include_directories(http_worker_pool_benchmark PRIVATE "${YUAN_LIBUV_ROOT}/include")
    target_link_libraries(http_worker_pool_benchmark PRIVATE "${YUAN_LIBUV_LIBRARY}")
    if (WIN32)
        target_link_libraries(http_worker_pool_benchmark PRIVATE ws2_32 iphlpapi psapi userenv dbghelp)
    else()
        target_link_libraries(http_worker_pool_benchmark PRIVATE pthread dl)
    endif()
endif()

set(YUAN_LIBEV_ROOT "${YUAN_NETWORK_REPOS_ROOT}/libev")
if (EXISTS "${YUAN_LIBEV_ROOT}/ev.h" AND EXISTS "${YUAN_LIBEV_ROOT}/ev.c")
    target_compile_definitions(http_worker_pool_benchmark PRIVATE YUAN_BENCH_WITH_LIBEV=1)
    target_include_directories(http_worker_pool_benchmark PRIVATE "${YUAN_LIBEV_ROOT}")
    target_compile_definitions(http_worker_pool_benchmark PRIVATE EV_STANDALONE=1)
    target_sources(http_worker_pool_benchmark PRIVATE "${YUAN_LIBEV_ROOT}/ev.c")
    if (UNIX AND NOT APPLE)
        target_link_libraries(http_worker_pool_benchmark PRIVATE m)
    endif()
endif()

set(YUAN_LIBHV_ROOT "${YUAN_NETWORK_REPOS_ROOT}/libhv")
set(YUAN_LIBHV_LIBRARY "")
option(YUAN_BENCH_ENABLE_LIBHV "Enable libhv benchmark implementation" ON)
if (YUAN_BENCH_ENABLE_LIBHV)
    if (EXISTS "${YUAN_LIBHV_ROOT}/build-release/lib/libhv.so")
        set(YUAN_LIBHV_LIBRARY "${YUAN_LIBHV_ROOT}/build-release/lib/libhv.so")
    elseif (EXISTS "${YUAN_LIBHV_ROOT}/build/lib/libhv.so")
        set(YUAN_LIBHV_LIBRARY "${YUAN_LIBHV_ROOT}/build/lib/libhv.so")
    elseif (EXISTS "${YUAN_LIBHV_ROOT}/lib/libhv.so")
        set(YUAN_LIBHV_LIBRARY "${YUAN_LIBHV_ROOT}/lib/libhv.so")
    elseif (EXISTS "${YUAN_LIBHV_ROOT}/build-release/lib/libhv_static.a")
        set(YUAN_LIBHV_LIBRARY "${YUAN_LIBHV_ROOT}/build-release/lib/libhv_static.a")
    elseif (EXISTS "${YUAN_LIBHV_ROOT}/build/lib/libhv_static.a")
        set(YUAN_LIBHV_LIBRARY "${YUAN_LIBHV_ROOT}/build/lib/libhv_static.a")
    elseif (EXISTS "${YUAN_LIBHV_ROOT}/lib/libhv_static.a")
        set(YUAN_LIBHV_LIBRARY "${YUAN_LIBHV_ROOT}/lib/libhv_static.a")
    elseif (EXISTS "${YUAN_LIBHV_ROOT}/lib/libhv.a")
        set(YUAN_LIBHV_LIBRARY "${YUAN_LIBHV_ROOT}/lib/libhv.a")
    endif()
endif()
if (YUAN_BENCH_ENABLE_LIBHV AND YUAN_LIBHV_LIBRARY)
    target_compile_definitions(http_worker_pool_benchmark PRIVATE YUAN_BENCH_WITH_LIBHV=1)
    target_include_directories(http_worker_pool_benchmark PRIVATE
        "${YUAN_LIBHV_ROOT}"
        "${YUAN_LIBHV_ROOT}/include"
        "${YUAN_LIBHV_ROOT}/base"
        "${YUAN_LIBHV_ROOT}/ssl"
        "${YUAN_LIBHV_ROOT}/event"
        "${YUAN_LIBHV_ROOT}/util"
        "${YUAN_LIBHV_ROOT}/cpputil"
        "${YUAN_LIBHV_ROOT}/evpp"
        "${YUAN_LIBHV_ROOT}/http"
        "${YUAN_LIBHV_ROOT}/http/server")
    target_link_libraries(http_worker_pool_benchmark PRIVATE "${YUAN_LIBHV_LIBRARY}")
    if (YUAN_LIBHV_LIBRARY MATCHES "\\.so$")
        get_filename_component(YUAN_LIBHV_LIBDIR "${YUAN_LIBHV_LIBRARY}" DIRECTORY)
        set_property(TARGET http_worker_pool_benchmark APPEND PROPERTY BUILD_RPATH "${YUAN_LIBHV_LIBDIR}")
    endif()
    if (WIN32)
        target_link_libraries(http_worker_pool_benchmark PRIVATE ws2_32 iphlpapi)
    else()
        target_link_libraries(http_worker_pool_benchmark PRIVATE pthread dl)
    endif()
endif()

# workflow and libhv both export http_parser symbols; avoid dual-link conflict
if (YUAN_LIBHV_LIBRARY)
    set(YUAN_BENCH_ENABLE_WORKFLOW_DEFAULT OFF)
else()
    set(YUAN_BENCH_ENABLE_WORKFLOW_DEFAULT ON)
endif()
option(YUAN_BENCH_ENABLE_WORKFLOW "Enable workflow benchmark implementation" ${YUAN_BENCH_ENABLE_WORKFLOW_DEFAULT})

set(YUAN_WORKFLOW_ROOT "${YUAN_NETWORK_REPOS_ROOT}/workflow")
if (YUAN_BENCH_ENABLE_WORKFLOW
    AND EXISTS "${YUAN_WORKFLOW_ROOT}/_lib/libworkflow.so"
    AND EXISTS "${YUAN_WORKFLOW_ROOT}/_include/workflow/WFHttpServer.h")
    target_compile_definitions(http_worker_pool_benchmark PRIVATE YUAN_BENCH_WITH_WORKFLOW=1)
    target_include_directories(http_worker_pool_benchmark PRIVATE "${YUAN_WORKFLOW_ROOT}/_include")
    target_link_libraries(http_worker_pool_benchmark PRIVATE
        "${YUAN_WORKFLOW_ROOT}/_lib/libworkflow.so")
    set_property(TARGET http_worker_pool_benchmark APPEND PROPERTY
        BUILD_RPATH "${YUAN_WORKFLOW_ROOT}/_lib")
endif()
