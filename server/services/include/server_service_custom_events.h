#ifndef __YUAN_SERVER_SERVICE_CUSTOM_EVENTS_H__
#define __YUAN_SERVER_SERVICE_CUSTOM_EVENTS_H__

#include "runtime_context.h"

#include <cstdint>
#include <string>

namespace yuan::server::events
{

    inline constexpr const char *http_request_received = "server.http.request.received";
    inline constexpr const char *http_request_completed = "server.http.request.completed";
    inline constexpr const char *http_request_error = "server.http.request.error";

    inline constexpr const char *dns_query_received = "server.dns.query.received";
    inline constexpr const char *dns_query_resolved = "server.dns.query.resolved";
    inline constexpr const char *dns_query_failed = "server.dns.query.failed";

    inline constexpr const char *ftp_session_connected = "server.ftp.session.connected";
    inline constexpr const char *ftp_session_disconnected = "server.ftp.session.disconnected";
    inline constexpr const char *ftp_transfer_completed = "server.ftp.transfer.completed";
    inline constexpr const char *ftp_transfer_failed = "server.ftp.transfer.failed";

    inline constexpr const char *socks5_session_connected = "server.socks5.session.connected";
    inline constexpr const char *socks5_session_relay_started = "server.socks5.session.relay_started";
    inline constexpr const char *socks5_session_relay_completed = "server.socks5.session.relay_completed";

    inline constexpr const char *proxy_session_accepted = "server.proxy.session.accepted";
    inline constexpr const char *proxy_session_rejected = "server.proxy.session.rejected";
    inline constexpr const char *proxy_session_completed = "server.proxy.session.completed";
    inline constexpr const char *proxy_session_state_changed = "server.proxy.session.state_changed";
    inline constexpr const char *proxy_session_snapshot = "server.proxy.session.snapshot";

    inline constexpr const char *websocket_session_connected = "server.websocket.session.connected";
    inline constexpr const char *websocket_session_disconnected = "server.websocket.session.disconnected";
    inline constexpr const char *websocket_message_received = "server.websocket.message.received";

    inline constexpr const char *bittorrent_peer_connected = "server.bittorrent.peer.connected";
    inline constexpr const char *bittorrent_piece_completed = "server.bittorrent.piece.completed";
    inline constexpr const char *bittorrent_torrent_completed = "server.bittorrent.torrent.completed";
}

namespace yuan::server
{

    struct HttpRequestEvent
    {
        std::string service_name;
        std::string method;
        std::string path;
        std::string remote_addr;
        uint16_t remote_port = 0;
    };

    struct HttpResponseEvent
    {
        std::string service_name;
        std::string method;
        std::string path;
        int status_code = 0;
        uint64_t duration_ms = 0;
        std::string remote_addr;
    };

    struct HttpErrorEvent
    {
        std::string service_name;
        std::string method;
        std::string path;
        std::string error;
        std::string remote_addr;
    };

    struct DnsQueryEvent
    {
        std::string service_name;
        std::string domain;
        uint16_t query_type = 0;
        std::string client_addr;
    };

    struct DnsResolvedEvent
    {
        std::string service_name;
        std::string domain;
        std::string result_ip;
        uint64_t duration_ms = 0;
    };

    struct DnsFailedEvent
    {
        std::string service_name;
        std::string domain;
        std::string error;
    };

    struct FtpSessionEvent
    {
        std::string service_name;
        std::string username;
        std::string remote_addr;
    };

    struct FtpTransferEvent
    {
        std::string service_name;
        std::string username;
        std::string path;
        std::string direction;
        uint64_t bytes_transferred = 0;
        uint64_t duration_ms = 0;
    };

    struct Socks5SessionEvent
    {
        std::string service_name;
        std::string username;
        std::string source_addr;
        std::string target_addr;
    };

    struct Socks5RelayEvent
    {
        std::string service_name;
        std::string source_addr;
        std::string target_addr;
        uint64_t bytes_up = 0;
        uint64_t bytes_down = 0;
        uint64_t duration_ms = 0;
    };

    struct ProxySessionAcceptedEvent
    {
        uint64_t session_id = 0;
        std::string service_name;
        std::string client_addr;
        std::string method;
        std::string target_addr;
        uint32_t active_sessions = 0;
    };

    struct ProxySessionRejectedEvent
    {
        uint64_t session_id = 0;
        std::string service_name;
        std::string client_addr;
        std::string method;
        std::string target_addr;
        std::string reason;
    };

    struct ProxySessionCompletedEvent
    {
        uint64_t session_id = 0;
        std::string service_name;
        std::string client_addr;
        std::string method;
        std::string target_addr;
        uint64_t duration_ms = 0;
        uint64_t bytes_up = 0;
        uint64_t bytes_down = 0;
        std::string close_reason;
    };

    struct ProxySessionStateChangedEvent
    {
        uint64_t session_id = 0;
        std::string service_name;
        std::string client_addr;
        std::string method;
        std::string target_addr;
        std::string previous_state;
        std::string current_state;
        std::string reason;
    };

    struct ProxySessionSnapshotEvent
    {
        std::string service_name;
        uint32_t accepted_sessions = 0;
        uint32_t reading_request_sessions = 0;
        uint32_t connecting_upstream_sessions = 0;
        uint32_t established_sessions = 0;
        uint32_t closing_sessions = 0;
        uint32_t active_sessions = 0;
        uint64_t total_accepted = 0;
        uint64_t total_rejected = 0;
        uint64_t total_completed = 0;
    };

    struct WebSocketSessionEvent
    {
        std::string service_name;
        std::string remote_addr;
    };

    struct WebSocketMessageEvent
    {
        std::string service_name;
        std::string remote_addr;
        std::size_t message_size = 0;
        bool is_binary = false;
    };

    struct BitTorrentPeerEvent
    {
        std::string service_name;
        std::string peer_id;
        std::string info_hash;
    };

    struct BitTorrentPieceEvent
    {
        std::string service_name;
        std::string info_hash;
        int piece_index = 0;
        std::size_t piece_size = 0;
    };

    struct BitTorrentTorrentEvent
    {
        std::string service_name;
        std::string info_hash;
        std::string name;
        std::size_t total_size = 0;
    };

} // namespace yuan::server

#endif
