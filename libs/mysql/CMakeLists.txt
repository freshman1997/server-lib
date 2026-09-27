cmake_minimum_required(VERSION 3.16)
project(YuanMysql)

option(YUAN_ENABLE_MYSQL "Build the MySQL client wrapper library" OFF)
option(YUAN_ENABLE_MYSQL_SMOKE_TEST "Build a manual MySQL smoke test executable" OFF)
option(YUAN_MYSQL_ALLOW_SYSTEM_CLIENT "Allow using an installed MySQL client library when source is not configured" OFF)
set(YUAN_MYSQL_CLIENT_SOURCE_DIR "" CACHE PATH "Path to MySQL client-capable source directory, such as mysql-server")
set(YUAN_MYSQL_OPENSSL_SOURCE_DIR "" CACHE PATH "Path to OpenSSL source/build directory used by MySQL source builds")
set(YUAN_MYSQL_OPENSSL_RUNTIME_DIR "" CACHE PATH "Path to OpenSSL runtime DLL directory used by MySQL source builds on Windows")

set(_yuan_mysql_source_candidates
    "${CMAKE_SOURCE_DIR}/third_party/mysql-server"
    "${CMAKE_SOURCE_DIR}/third_party/mysql-server-8.4.10"
    "${CMAKE_SOURCE_DIR}/third_party/mysql-8.4.10"
    "${CMAKE_SOURCE_DIR}/mysql-server"
    "${CMAKE_SOURCE_DIR}/mysql-server-8.4.10"
    "${CMAKE_SOURCE_DIR}/mysql-8.4.10"
    "${CMAKE_SOURCE_DIR}/../mysql-server"
    "${CMAKE_SOURCE_DIR}/../mysql-server-8.4.10"
    "${CMAKE_SOURCE_DIR}/../mysql-8.4.10"
    "${CMAKE_BINARY_DIR}/mysql-server"
    "${CMAKE_BINARY_DIR}/mysql-server-8.4.10"
    "${CMAKE_BINARY_DIR}/mysql-8.4.10")

if(NOT YUAN_MYSQL_CLIENT_SOURCE_DIR)
    foreach(_yuan_mysql_source_candidate IN LISTS _yuan_mysql_source_candidates)
        if(EXISTS "${_yuan_mysql_source_candidate}/CMakeLists.txt")
            set(YUAN_MYSQL_CLIENT_SOURCE_DIR "${_yuan_mysql_source_candidate}")
            break()
        endif()
    endforeach()
endif()

if(NOT YUAN_ENABLE_MYSQL)
    return()
endif()

if(NOT EXISTS "${YUAN_MYSQL_CLIENT_SOURCE_DIR}/CMakeLists.txt" AND NOT YUAN_MYSQL_ALLOW_SYSTEM_CLIENT)
    message(FATAL_ERROR
        "YUAN_ENABLE_MYSQL is ON, but YUAN_MYSQL_CLIENT_SOURCE_DIR does not point to MySQL client sources. "
        "Add mysql-server sources under third_party/mysql-server, or configure "
        "-DYUAN_MYSQL_CLIENT_SOURCE_DIR=/path/to/mysql-server")
endif()

if(EXISTS "${YUAN_MYSQL_CLIENT_SOURCE_DIR}/CMakeLists.txt")
    include(ExternalProject)

    # Keep compiler-specific fixes out of the checked-out third-party source.
    set(_yuan_mysql_source_dir "${CMAKE_BINARY_DIR}/_deps/mysql-client-source")
    file(REMOVE_RECURSE "${_yuan_mysql_source_dir}")
    file(COPY "${YUAN_MYSQL_CLIENT_SOURCE_DIR}/" DESTINATION "${_yuan_mysql_source_dir}")

    set(_yuan_mysql_build_dir "${CMAKE_BINARY_DIR}/_deps/mysql-client-build")
    set(_yuan_mysql_install_dir "${CMAKE_BINARY_DIR}/_deps/mysql-client-install")
    file(MAKE_DIRECTORY "${_yuan_mysql_source_dir}/include" "${_yuan_mysql_build_dir}/include")

    if(CMAKE_BUILD_TYPE)
        set(_yuan_mysql_build_type "${CMAKE_BUILD_TYPE}")
    else()
        set(_yuan_mysql_build_type "RelWithDebInfo")
    endif()

    set(_yuan_mysql_client_c_flags "${CMAKE_C_FLAGS}")
    set(_yuan_mysql_client_cxx_flags "${CMAKE_CXX_FLAGS}")
    if(MINGW)
        string(APPEND _yuan_mysql_client_c_flags " -Wno-error=incompatible-pointer-types")
        string(APPEND _yuan_mysql_client_c_flags " -DZSTD_DISABLE_ASM")
        string(APPEND _yuan_mysql_client_cxx_flags " -DZSTD_DISABLE_ASM")
    endif()

    if(NOT YUAN_MYSQL_OPENSSL_SOURCE_DIR)
        foreach(_yuan_mysql_openssl_candidate IN ITEMS
            "${CMAKE_SOURCE_DIR}/third_party/openssl-3.4.0"
            "${CMAKE_SOURCE_DIR}/third_party/openssl"
            "${CMAKE_SOURCE_DIR}/../openssl-3.4.0"
            "${CMAKE_SOURCE_DIR}/../openssl")
            if(EXISTS "${_yuan_mysql_openssl_candidate}/include/openssl/ssl.h" AND
               EXISTS "${_yuan_mysql_openssl_candidate}/${CMAKE_STATIC_LIBRARY_PREFIX}ssl${CMAKE_STATIC_LIBRARY_SUFFIX}" AND
               EXISTS "${_yuan_mysql_openssl_candidate}/${CMAKE_STATIC_LIBRARY_PREFIX}crypto${CMAKE_STATIC_LIBRARY_SUFFIX}")
                set(YUAN_MYSQL_OPENSSL_SOURCE_DIR "${_yuan_mysql_openssl_candidate}")
                break()
            endif()
        endforeach()
    endif()

    set(_yuan_mysql_cmake_args
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DCMAKE_BUILD_TYPE=${_yuan_mysql_build_type}
        -DCMAKE_C_FLAGS=${_yuan_mysql_client_c_flags}
        -DCMAKE_CXX_FLAGS=${_yuan_mysql_client_cxx_flags}
        -DWITHOUT_SERVER=ON
        -DWITH_UNIT_TESTS=OFF
        -DWITH_TESTS=OFF
        -DWITH_DOCS=OFF)

    if(EXISTS "${_yuan_mysql_source_dir}/sql/auth/sha2_password_common.cc")
        set(_yuan_mysql_is_server_source TRUE)
        if(NOT YUAN_MYSQL_OPENSSL_SOURCE_DIR)
            message(FATAL_ERROR
                "MySQL Server source builds require OpenSSL. Configure "
                "-DYUAN_MYSQL_OPENSSL_SOURCE_DIR=/path/to/openssl or provide third_party/openssl-3.4.0")
        endif()

        set(_yuan_mysql_openssl_stage_dir "${CMAKE_BINARY_DIR}/_deps/openssl-for-mysql")
        file(REMOVE_RECURSE "${_yuan_mysql_openssl_stage_dir}")
        file(MAKE_DIRECTORY "${_yuan_mysql_openssl_stage_dir}/lib" "${_yuan_mysql_openssl_stage_dir}/ms")
        file(COPY "${YUAN_MYSQL_OPENSSL_SOURCE_DIR}/include" DESTINATION "${_yuan_mysql_openssl_stage_dir}")
        file(COPY "${YUAN_MYSQL_OPENSSL_SOURCE_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}ssl${CMAKE_STATIC_LIBRARY_SUFFIX}"
             DESTINATION "${_yuan_mysql_openssl_stage_dir}/lib")
        file(COPY "${YUAN_MYSQL_OPENSSL_SOURCE_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}crypto${CMAKE_STATIC_LIBRARY_SUFFIX}"
             DESTINATION "${_yuan_mysql_openssl_stage_dir}/lib")
        if(EXISTS "${YUAN_MYSQL_OPENSSL_SOURCE_DIR}/ms/applink.c")
            file(COPY "${YUAN_MYSQL_OPENSSL_SOURCE_DIR}/ms/applink.c" DESTINATION "${_yuan_mysql_openssl_stage_dir}/ms")
            file(COPY "${YUAN_MYSQL_OPENSSL_SOURCE_DIR}/ms/applink.c" DESTINATION "${_yuan_mysql_openssl_stage_dir}/include/openssl")
            if(MINGW)
                set(_yuan_mysql_openssl_applink "${_yuan_mysql_openssl_stage_dir}/include/openssl/applink.c")
                file(READ "${_yuan_mysql_openssl_applink}" _yuan_mysql_openssl_applink_content)
                string(REPLACE "static void *OPENSSL_ApplinkTable[APPLINK_MAX + 1] = { (void *)APPLINK_MAX };"
                    "#ifdef __cplusplus\n#define YUAN_OPENSSL_APPLINK_CAST(value) reinterpret_cast<void *>(value)\n#else\n#define YUAN_OPENSSL_APPLINK_CAST(value) (void *)(value)\n#endif\n    static void *OPENSSL_ApplinkTable[APPLINK_MAX + 1] = { (void *)APPLINK_MAX };"
                    _yuan_mysql_openssl_applink_content "${_yuan_mysql_openssl_applink_content}")
                foreach(_yuan_mysql_openssl_applink_symbol IN ITEMS
                    app_stdin app_stdout app_stderr fprintf fgets fread fwrite app_fsetmod app_feof fclose
                    fopen fseek ftell fflush app_ferror app_clearerr app_fileno _open _read _write _lseek _close)
                    string(REPLACE "= ${_yuan_mysql_openssl_applink_symbol};" "= YUAN_OPENSSL_APPLINK_CAST(${_yuan_mysql_openssl_applink_symbol});"
                        _yuan_mysql_openssl_applink_content "${_yuan_mysql_openssl_applink_content}")
                endforeach()
                file(WRITE "${_yuan_mysql_openssl_applink}" "${_yuan_mysql_openssl_applink_content}")
            endif()
        endif()

        if(WIN32)
            if(NOT YUAN_MYSQL_OPENSSL_RUNTIME_DIR)
                foreach(_yuan_mysql_openssl_runtime_candidate IN ITEMS
                    "${YUAN_MYSQL_OPENSSL_SOURCE_DIR}/bin"
                    "${YUAN_MYSQL_OPENSSL_SOURCE_DIR}")
                    if(EXISTS "${_yuan_mysql_openssl_runtime_candidate}/libssl-3-x64.dll" AND
                       EXISTS "${_yuan_mysql_openssl_runtime_candidate}/libcrypto-3-x64.dll")
                        set(YUAN_MYSQL_OPENSSL_RUNTIME_DIR "${_yuan_mysql_openssl_runtime_candidate}")
                        break()
                    endif()
                endforeach()
            endif()

            if(YUAN_MYSQL_OPENSSL_RUNTIME_DIR)
                file(MAKE_DIRECTORY "${_yuan_mysql_openssl_stage_dir}/bin")
                file(COPY "${YUAN_MYSQL_OPENSSL_RUNTIME_DIR}/libssl-3-x64.dll"
                     DESTINATION "${_yuan_mysql_openssl_stage_dir}/bin")
                file(COPY "${YUAN_MYSQL_OPENSSL_RUNTIME_DIR}/libcrypto-3-x64.dll"
                     DESTINATION "${_yuan_mysql_openssl_stage_dir}/bin")
            endif()
        endif()

        list(APPEND _yuan_mysql_cmake_args
            -DCMAKE_CXX_STANDARD=20
            -DCMAKE_CXX_STANDARD_REQUIRED=ON
            -DWITH_ROUTER=OFF
            -DWITH_NDB=OFF
            -DWITH_ZLIB=bundled
            -DWITH_ZSTD=bundled
            -DWITH_SSL=${_yuan_mysql_openssl_stage_dir})

        if(MINGW)
            list(APPEND _yuan_mysql_cmake_args
                "-DCMAKE_EXE_LINKER_FLAGS=-lws2_32 -lwsock32 -lcrypt32 -lbcrypt -ladvapi32 -luser32"
                "-DCMAKE_SHARED_LINKER_FLAGS=-lws2_32 -lwsock32 -lcrypt32 -lbcrypt -ladvapi32 -luser32"
                "-DCMAKE_MODULE_LINKER_FLAGS=-lws2_32 -lwsock32 -lcrypt32 -lbcrypt -ladvapi32 -luser32")
        endif()
    endif()

    if(_yuan_mysql_is_server_source AND MINGW)
        set(_yuan_mysql_patch_command
            ${CMAKE_COMMAND}
            -DMYSQL_SOURCE_DIR=${_yuan_mysql_source_dir}
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/patch_mysql_server_mingw.cmake)
    else()
        set(_yuan_mysql_patch_command "")
    endif()

    set(_yuan_mysql_library_candidates
        "${_yuan_mysql_build_dir}/archive_output_directory/${CMAKE_STATIC_LIBRARY_PREFIX}mysqlclient${CMAKE_STATIC_LIBRARY_SUFFIX}"
        "${_yuan_mysql_build_dir}/libmysql/${CMAKE_STATIC_LIBRARY_PREFIX}mysqlclient${CMAKE_STATIC_LIBRARY_SUFFIX}"
        "${_yuan_mysql_build_dir}/libmysql/${CMAKE_STATIC_LIBRARY_PREFIX}libmysql${CMAKE_STATIC_LIBRARY_SUFFIX}"
        "${_yuan_mysql_build_dir}/client/${CMAKE_STATIC_LIBRARY_PREFIX}mysqlclient${CMAKE_STATIC_LIBRARY_SUFFIX}"
        "${_yuan_mysql_build_dir}/client/${CMAKE_STATIC_LIBRARY_PREFIX}libmysql${CMAKE_STATIC_LIBRARY_SUFFIX}")
    if(_yuan_mysql_is_server_source)
        set(_yuan_mysql_library "${_yuan_mysql_build_dir}/archive_output_directory/${CMAKE_STATIC_LIBRARY_PREFIX}mysqlclient${CMAKE_STATIC_LIBRARY_SUFFIX}")
    else()
        set(_yuan_mysql_library "${_yuan_mysql_build_dir}/libmysql/${CMAKE_STATIC_LIBRARY_PREFIX}mysqlclient${CMAKE_STATIC_LIBRARY_SUFFIX}")
    endif()

    ExternalProject_Add(yuan_mysql_client_project
        SOURCE_DIR "${_yuan_mysql_source_dir}"
        BINARY_DIR "${_yuan_mysql_build_dir}"
        INSTALL_DIR "${_yuan_mysql_install_dir}"
        PATCH_COMMAND ${_yuan_mysql_patch_command}
        CMAKE_ARGS ${_yuan_mysql_cmake_args}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target mysqlclient
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS ${_yuan_mysql_library_candidates})

    add_library(yuan_mysqlclient STATIC IMPORTED GLOBAL)
    set_target_properties(yuan_mysqlclient PROPERTIES
        IMPORTED_LOCATION "${_yuan_mysql_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${_yuan_mysql_source_dir}/include;${_yuan_mysql_build_dir}/include")
    add_dependencies(yuan_mysqlclient yuan_mysql_client_project)
else()
    find_path(_yuan_mysql_include_dir mysql.h PATH_SUFFIXES mysql)
    find_library(_yuan_mysql_library NAMES mysqlclient libmysql)
    if(NOT _yuan_mysql_include_dir OR NOT _yuan_mysql_library)
        message(FATAL_ERROR "YUAN_MYSQL_ALLOW_SYSTEM_CLIENT is ON, but mysql.h/mysqlclient were not found")
    endif()
    add_library(yuan_mysqlclient UNKNOWN IMPORTED GLOBAL)
    set_target_properties(yuan_mysqlclient PROPERTIES
        IMPORTED_LOCATION "${_yuan_mysql_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${_yuan_mysql_include_dir}")
endif()

add_library(YuanMysql src/mysql_client.cpp src/mysql_async_client.cpp src/mysql_orm.cpp src/mysql_db_proxy.cpp)
add_library(yuan_mysql ALIAS YuanMysql)

target_compile_features(YuanMysql PUBLIC cxx_std_20)
target_include_directories(YuanMysql PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(YuanMysql PUBLIC yuan_mysqlclient Core)

if(WIN32)
    target_link_libraries(YuanMysql PUBLIC ws2_32 crypt32 secur32 advapi32 bcrypt shlwapi)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(YuanMysql PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

if(YUAN_ENABLE_MYSQL_SMOKE_TEST)
    add_executable(YuanMysqlSmokeTest test/mysql_smoke_test.cpp)
    target_link_libraries(YuanMysqlSmokeTest PRIVATE YuanMysql)

    add_executable(YuanMysqlAsyncSmokeTest test/mysql_async_smoke_test.cpp)
    target_link_libraries(YuanMysqlAsyncSmokeTest PRIVATE YuanMysql)

    add_executable(YuanMysqlDbProxySmokeTest test/mysql_db_proxy_smoke_test.cpp)
    target_link_libraries(YuanMysqlDbProxySmokeTest PRIVATE YuanMysql)
endif()

unset(_yuan_mysql_build_dir)
unset(_yuan_mysql_source_candidate)
unset(_yuan_mysql_source_candidates)
unset(_yuan_mysql_install_dir)
unset(_yuan_mysql_build_type)
unset(_yuan_mysql_client_c_flags)
unset(_yuan_mysql_client_cxx_flags)
unset(_yuan_mysql_cmake_args)
unset(_yuan_mysql_library)
unset(_yuan_mysql_library_candidates)
unset(_yuan_mysql_is_server_source)
unset(_yuan_mysql_patch_command)
unset(_yuan_mysql_openssl_candidate)
unset(_yuan_mysql_openssl_runtime_candidate)
unset(_yuan_mysql_openssl_stage_dir)
