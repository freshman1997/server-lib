option(YUAN_TEST_BUILD_PROTOCOL_HTTP "Build HTTP protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_DNS "Build DNS protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_BIT_TORRENT "Build BitTorrent protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_MQTT "Build MQTT protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_WEBSOCKET "Build WebSocket protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_FTP "Build FTP protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_SOCKS5 "Build SOCKS5 protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_SSH "Build SSH protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_SMB "Build SMB protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_SHADOWSOCKS "Build Shadowsocks protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_WEBDAV "Build WebDAV protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_RTC "Build RTC protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_RTCP "Build RTCP protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_WEBRTC "Build WebRTC protocol tests" ON)
option(YUAN_TEST_BUILD_PROTOCOL_RTSP "Build RTSP protocol tests" ON)

if (YUAN_TEST_BUILD_PROTOCOL_HTTP)
    add_subdirectory(http)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_DNS)
    add_subdirectory(dns)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_BIT_TORRENT)
    add_subdirectory(bit_torrent)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_MQTT)
    add_subdirectory(mqtt)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_WEBSOCKET)
    add_subdirectory(websocket)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_FTP)
    add_subdirectory(ftp)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_SOCKS5)
    add_subdirectory(socks5)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_SSH AND TARGET SshProto)
    add_subdirectory(ssh)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_SMB)
    add_subdirectory(smb)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_SHADOWSOCKS)
    add_subdirectory(shadowsocks)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_WEBDAV)
    add_subdirectory(webdav)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_RTC)
    add_subdirectory(rtc)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_RTCP)
    add_subdirectory(rtcp)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_WEBRTC)
    add_subdirectory(webrtc)
endif()

if (YUAN_TEST_BUILD_PROTOCOL_RTSP)
    add_subdirectory(rtsp)
endif()
