cmake_minimum_required(VERSION 3.12)
project(release_targets)

add_subdirectory(filesync)

if (TARGET SshProto)
    add_subdirectory(ssh)
endif()
