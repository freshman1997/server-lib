add_executable(test_webdav test_webdav.cpp)
target_link_libraries(test_webdav PUBLIC WebDavProto HttpProto)

if (BUILD_TESTING)
    add_test(NAME webdav COMMAND test_webdav)
endif()
