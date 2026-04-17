#ifndef __YUAN_SERVER_SERVICE_EVENT_TYPE_REGISTRATION_H__
#define __YUAN_SERVER_SERVICE_EVENT_TYPE_REGISTRATION_H__

#include "eventbus/event_type_registry.h"
#include "server_service_custom_events.h"

namespace yuan::server
{

    inline void register_service_event_types(eventbus::EventTypeRegistry & registry)
    {
        using namespace eventbus;
        using namespace events;

        registry.register_type(http_request_received, "http", "HttpRequestEvent",
                               "HTTP request received by server", EventScope::service);
        registry.register_type(http_request_completed, "http", "HttpResponseEvent",
                               "HTTP request completed", EventScope::service);
        registry.register_type(http_request_error, "http", "HttpErrorEvent",
                               "HTTP request errored", EventScope::service);

        registry.register_type(dns_query_received, "dns", "DnsQueryEvent",
                               "DNS query received by server", EventScope::service);
        registry.register_type(dns_query_resolved, "dns", "DnsResolvedEvent",
                               "DNS query resolved", EventScope::service);
        registry.register_type(dns_query_failed, "dns", "DnsFailedEvent",
                               "DNS query failed", EventScope::service);

        registry.register_type(ftp_session_connected, "ftp", "FtpSessionEvent",
                               "FTP client session connected", EventScope::service);
        registry.register_type(ftp_session_disconnected, "ftp", "FtpSessionEvent",
                               "FTP client session disconnected", EventScope::service);
        registry.register_type(ftp_transfer_completed, "ftp", "FtpTransferEvent",
                               "FTP file transfer completed", EventScope::service);
        registry.register_type(ftp_transfer_failed, "ftp", "FtpTransferEvent",
                               "FTP file transfer failed", EventScope::service);

        registry.register_type(socks5_session_connected, "socks5", "Socks5SessionEvent",
                               "SOCKS5 client session connected", EventScope::service);
        registry.register_type(socks5_session_relay_started, "socks5", "Socks5SessionEvent",
                               "SOCKS5 relay started", EventScope::service);
        registry.register_type(socks5_session_relay_completed, "socks5", "Socks5RelayEvent",
                               "SOCKS5 relay completed", EventScope::service);

        registry.register_type(websocket_session_connected, "websocket", "WebSocketSessionEvent",
                               "WebSocket session connected", EventScope::service);
        registry.register_type(websocket_session_disconnected, "websocket", "WebSocketSessionEvent",
                               "WebSocket session disconnected", EventScope::service);
        registry.register_type(websocket_message_received, "websocket", "WebSocketMessageEvent",
                               "WebSocket message received", EventScope::service);

        registry.register_type(bittorrent_peer_connected, "bittorrent", "BitTorrentPeerEvent",
                               "BitTorrent peer connected", EventScope::service);
        registry.register_type(bittorrent_piece_completed, "bittorrent", "BitTorrentPieceEvent",
                               "BitTorrent piece download completed", EventScope::service);
        registry.register_type(bittorrent_torrent_completed, "bittorrent", "BitTorrentTorrentEvent",
                               "BitTorrent torrent download completed", EventScope::service);

        registry.register_type(service_activating, "lifecycle", "ServiceRuntimeEvent",
                               "Service is activating", EventScope::global);
        registry.register_type(service_activated, "lifecycle", "ServiceRuntimeEvent",
                               "Service is activated", EventScope::global);
        registry.register_type(service_stopping, "lifecycle", "ServiceRuntimeEvent",
                               "Service is stopping", EventScope::global);
        registry.register_type(service_stopped, "lifecycle", "ServiceRuntimeEvent",
                               "Service is stopped", EventScope::global);
    }

} // namespace yuan::server

#endif
