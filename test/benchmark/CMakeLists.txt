add_executable(core_runtime_benchmark core_runtime_benchmark.cpp)
target_link_libraries(core_runtime_benchmark PUBLIC Core App)

add_executable(http_worker_pool_benchmark http_worker_pool_benchmark.cpp)
target_link_libraries(http_worker_pool_benchmark PUBLIC HttpProto ServerServiceHttp)

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

set(YUAN_WORKFLOW_ROOT "${YUAN_NETWORK_REPOS_ROOT}/workflow")
if (EXISTS "${YUAN_WORKFLOW_ROOT}/_lib/libworkflow.so"
    AND EXISTS "${YUAN_WORKFLOW_ROOT}/_include/workflow/WFHttpServer.h")
    target_compile_definitions(http_worker_pool_benchmark PRIVATE YUAN_BENCH_WITH_WORKFLOW=1)
    target_include_directories(http_worker_pool_benchmark PRIVATE "${YUAN_WORKFLOW_ROOT}/_include")
    target_link_libraries(http_worker_pool_benchmark PRIVATE
        "${YUAN_WORKFLOW_ROOT}/_lib/libworkflow.so")
    set_property(TARGET http_worker_pool_benchmark APPEND PROPERTY
        BUILD_RPATH "${YUAN_WORKFLOW_ROOT}/_lib")
endif()
