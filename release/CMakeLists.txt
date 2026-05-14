cmake_minimum_required(VERSION 3.12)
project(release_targets)

add_subdirectory(filesync)
add_subdirectory(dns)

if (TARGET FtpProto)
    add_subdirectory(ftp)
endif()

if (TARGET SshProto)
    add_subdirectory(ssh)
endif()

if (TARGET MqttProto)
    add_subdirectory(mqtt)
endif()

if (TARGET RtspProto)
    add_subdirectory(rtsp)
endif()

if (TARGET WebrtcProto)
    add_subdirectory(webrtc)
endif()
