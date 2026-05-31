cmake_minimum_required(VERSION 3.12)
project(ServerServices)

set(SERVER_SERVICES_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

function(yuan_add_server_service_library target_name)
    add_library(${target_name} ${ARGN})
    target_compile_features(${target_name} PUBLIC cxx_std_20)
    target_include_directories(${target_name} PUBLIC
        "${CMAKE_SOURCE_DIR}"
        "${SERVER_SERVICES_INCLUDE_DIR}"
    )
endfunction()

yuan_add_server_service_library(ServerServiceBitTorrent
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bit_torrent/bit_torrent_service.cpp"
)
target_link_libraries(ServerServiceBitTorrent PUBLIC ServerProxy BitTorrentProto)

yuan_add_server_service_library(ServerServiceDns
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dns/dns_service.cpp"
)
target_link_libraries(ServerServiceDns PUBLIC ServerProxy DnsProto)

yuan_add_server_service_library(ServerServiceHttp
    "${CMAKE_CURRENT_SOURCE_DIR}/src/http/http_service.cpp"
)
target_link_libraries(ServerServiceHttp PUBLIC ServerProxy HttpProto ServerServiceBitTorrent)

yuan_add_server_service_library(ServerServiceMqtt
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mqtt/mqtt_service.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mqtt/mqtt_service_enhanced_handler.cpp"
)
target_link_libraries(ServerServiceMqtt PUBLIC ServerProxy MqttProto)

yuan_add_server_service_library(ServerServiceSocks5
    "${CMAKE_CURRENT_SOURCE_DIR}/src/socks5/socks5_service.cpp"
)
target_link_libraries(ServerServiceSocks5 PUBLIC ServerProxy Socks5Proto)

yuan_add_server_service_library(ServerServiceWebSocket
    "${CMAKE_CURRENT_SOURCE_DIR}/src/websocket/websocket_service.cpp"
)
target_link_libraries(ServerServiceWebSocket PUBLIC ServerProxy WebSocketProto)

if (TARGET RtspProto)
    yuan_add_server_service_library(ServerServiceRtsp
        "${CMAKE_CURRENT_SOURCE_DIR}/src/rtsp/rtsp_service.cpp"
    )
    target_link_libraries(ServerServiceRtsp PUBLIC ServerProxy RtspProto)
endif()

if (TARGET FtpProto)
    yuan_add_server_service_library(ServerServiceFtp
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ftp/ftp_service.cpp"
    )
    target_link_libraries(ServerServiceFtp PUBLIC ServerProxy FtpProto)
endif()

if (TARGET SshProto)
    yuan_add_server_service_library(ServerServiceSsh
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ssh/ssh_service.cpp"
    )
    target_link_libraries(ServerServiceSsh PUBLIC ServerProxy SshProto)
endif()

if (TARGET SmbProto)
    yuan_add_server_service_library(ServerServiceSmb
        "${CMAKE_CURRENT_SOURCE_DIR}/src/smb/smb_service.cpp"
    )
    target_link_libraries(ServerServiceSmb PUBLIC ServerProxy SmbProto)

    if (TARGET NasCore)
        yuan_add_server_service_library(ServerServiceNasSmbAdapter
            "${CMAKE_CURRENT_SOURCE_DIR}/src/nas/nas_smb_adapter.cpp"
        )
        target_link_libraries(ServerServiceNasSmbAdapter PUBLIC SmbProto NasCore)
    endif()
endif()

if (TARGET NasCore)
    yuan_add_server_service_library(ServerServiceNas
        "${CMAKE_CURRENT_SOURCE_DIR}/src/nas/nas_service.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/nas/nas_service_config.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/nas/nas_service_readiness.cpp"
    )
    target_link_libraries(ServerServiceNas PUBLIC NasCore ServerServiceHttp)
    if (TARGET ServerServiceSmb)
        target_link_libraries(ServerServiceNas PUBLIC ServerServiceSmb)
    endif()
    if (TARGET ServerServiceNasSmbAdapter)
        target_link_libraries(ServerServiceNas PUBLIC ServerServiceNasSmbAdapter)
    endif()
endif()

add_library(ServerServices INTERFACE)
target_include_directories(ServerServices INTERFACE
    "${CMAKE_SOURCE_DIR}"
    "${SERVER_SERVICES_INCLUDE_DIR}"
)
target_link_libraries(ServerServices INTERFACE
    ServerProxy
    ServerServiceBitTorrent
    ServerServiceDns
    ServerServiceHttp
    ServerServiceMqtt
    ServerServiceSocks5
    ServerServiceWebSocket
)

if (TARGET ServerServiceRtsp)
    target_link_libraries(ServerServices INTERFACE ServerServiceRtsp)
endif()

if (TARGET ServerServiceFtp)
    target_link_libraries(ServerServices INTERFACE ServerServiceFtp)
endif()

if (TARGET ServerServiceSsh)
    target_link_libraries(ServerServices INTERFACE ServerServiceSsh)
endif()

if (TARGET ServerServiceSmb)
    target_link_libraries(ServerServices INTERFACE ServerServiceSmb)
endif()

if (TARGET ServerServiceNasSmbAdapter)
    target_link_libraries(ServerServices INTERFACE ServerServiceNasSmbAdapter)
endif()

if (TARGET ServerServiceNas)
    target_link_libraries(ServerServices INTERFACE ServerServiceNas)
endif()

install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/nas_admin_console.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/nas_file_console.html"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/yuan/server/services/resources"
)
