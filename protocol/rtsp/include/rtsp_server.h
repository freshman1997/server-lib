#ifndef __NET_RTSP_SERVER_H__
#define __NET_RTSP_SERVER_H__

#include "net/async/async_connection_context.h"
#include "net/async/async_listener_host.h"
#include "net/runtime/network_runtime.h"
#include "rtcp_packet.h"
#include "rtcp_session.h"
#include "rtsp_request.h"
#include "rtsp_response.h"
#include "rtsp_session.h"
#include "rtc_packet.h"
#include "rtp_session.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace yuan::net::rtsp
{

struct InterleavedStatsSnapshot
{
    uint64_t handled_rtp = 0;
    uint64_t handled_rtcp = 0;
    uint64_t unknown_channel = 0;
    uint64_t malformed_rtp = 0;
    uint64_t malformed_rtcp = 0;
    uint64_t session_expired = 0;
};

struct RtspAclConfig
{
    bool enabled = false;
    bool default_allow = true;
    std::vector<std::string> allow_ips;
    std::vector<std::string> deny_ips;
    std::vector<std::string> allow_cidrs;
    std::vector<std::string> deny_cidrs;
    std::vector<std::string> allow_uri_prefixes;
    std::vector<std::string> deny_uri_prefixes;
};

struct RtspRateLimitConfig
{
    bool enabled = false;
    uint32_t max_requests = 0;
    uint64_t window_ms = 1000;
    uint32_t auth_fail_limit = 0;
    uint64_t auth_ban_ms = 0;
};

struct SecurityStatsSnapshot
{
    uint64_t acl_denied = 0;
    uint64_t rate_limited = 0;
    uint64_t auth_banned = 0;
    uint64_t auth_basic_fail = 0;
    uint64_t auth_digest_fail = 0;
    uint64_t auth_success = 0;
};

struct RtcpBridgeSnapshot
{
    bool has_sender_activity = false;
    uint64_t sender_ntp_timestamp = 0;
    uint32_t sender_rtp_timestamp = 0;
    uint32_t sender_packet_count = 0;
    uint32_t sender_octet_count = 0;
    uint32_t last_sr_lsr = 0;
    uint32_t last_sr_delay_65536 = 0;
    uint64_t rr_reports_built = 0;
    uint64_t sr_reports_built = 0;
};

struct RtspMediaBridgeSnapshot
{
    std::size_t rtp_session_count = 0;
    uint64_t rejected_rtp_packets = 0;
    RtcpBridgeSnapshot rtcp;
};

struct RtspInterleavedFrame
{
    uint8_t channel = 0;
    std::string bytes;
};

struct RtspMetricsSnapshot
{
    uint64_t requests_total = 0;
    uint64_t parse_errors = 0;
    uint64_t responses_2xx = 0;
    uint64_t responses_4xx = 0;
    uint64_t responses_5xx = 0;
    uint64_t outbound_interleaved_sent = 0;
    uint64_t outbound_udp_sent = 0;
    uint64_t outbound_udp_failed = 0;
    uint64_t outbound_udp_retried = 0;
    uint64_t outbound_udp_dropped = 0;
    std::map<std::string, uint64_t> outbound_udp_failed_by_track;
    std::map<std::string, uint64_t> method_counts;
    std::map<uint16_t, uint64_t> status_counts;
};

struct RtspAuditEvent
{
    uint64_t timestamp_ms = 0;
    std::string client_ip;
    std::string session_id;
    std::string method;
    uint16_t status_code = 0;
    std::string action;
    std::string result;
    std::string detail;
};

struct RtspObservabilityConfig
{
    bool enable_log = false;
    bool enable_audit = true;
    std::size_t max_audit_events = 256;
    uint32_t udp_retry_max_retries = 2;
    uint64_t udp_retry_base_backoff_ms = 25;
    uint64_t udp_retry_max_backoff_ms = 1000;
};

enum class RtspOutboundTransport
{
    interleaved_tcp,
    udp_unicast,
};

struct RtspOutboundPacket
{
    RtspOutboundTransport transport = RtspOutboundTransport::interleaved_tcp;
    std::string session_id;
    std::string track_uri;
    bool is_rtcp = true;
    uint8_t interleaved_channel = 0;
    int udp_remote_port = -1;
    uint32_t retry_count = 0;
    uint32_t max_retries = 2;
    uint64_t next_retry_after_ms = 0;
    std::string bytes;
};

} // namespace yuan::net::rtsp

namespace yuan::net::rtsp
{

class RtspServer
{
public:
    using RequestHandler = std::function<void(const RtspRequest &, RtspResponse &)>;

    enum class InterleavedFrameResult
    {
        handled_rtp,
        handled_rtcp,
        unknown_channel,
        malformed_rtp,
        malformed_rtcp,
        session_expired,
    };

    RtspServer();
    ~RtspServer();

    bool init(int port);
    bool init(int port, NetworkRuntime &runtime);
    void serve();
    void stop();

    void set_handler(RequestHandler handler);
    RtspResponse handle_request(const RtspRequest &request);
    std::size_t session_count() const;

    void configure_basic_auth(std::string realm, std::string username, std::string password);
    void configure_digest_auth(std::string realm, std::string username, std::string password);
    void clear_basic_auth();
    void clear_digest_auth();
    void configure_acl(const RtspAclConfig &config);
    void clear_acl();
    void configure_rate_limit(const RtspRateLimitConfig &config);
    void clear_rate_limit();

    bool inject_rtp_packet(const std::string &session_id, const ::yuan::net::rtc::RtcPacket &packet, uint64_t arrival_time_ms);
    bool build_receiver_report(const std::string &session_id, ::yuan::net::rtcp::RtcpPacket &out_packet);
    bool build_sender_report(const std::string &session_id, ::yuan::net::rtcp::RtcpPacket &out_packet);
    bool build_interleaved_rtp_frame(
        const std::string &session_id,
        const std::string &track_uri,
        const ::yuan::net::rtc::RtcPacket &packet,
        RtspInterleavedFrame &out_frame);
    bool build_interleaved_receiver_report_frame(
        const std::string &session_id,
        const std::string &track_uri,
        RtspInterleavedFrame &out_frame);
    bool build_interleaved_sender_report_frame(
        const std::string &session_id,
        const std::string &track_uri,
        RtspInterleavedFrame &out_frame);
    bool media_bridge_snapshot(const std::string &session_id, RtspMediaBridgeSnapshot &out_snapshot) const;
    InterleavedFrameResult handle_interleaved_frame(uint8_t channel, std::string_view payload, uint64_t arrival_time_ms);
    InterleavedStatsSnapshot interleaved_stats_snapshot() const;
    SecurityStatsSnapshot security_stats_snapshot() const;
    RtspMetricsSnapshot metrics_snapshot() const;
    std::vector<RtspAuditEvent> recent_audit_events(std::size_t max_events) const;
    void configure_observability(const RtspObservabilityConfig &config);
    std::vector<RtspOutboundPacket> drain_outbound_packets(std::size_t max_packets);
    std::size_t outbound_packet_count() const;
    std::size_t flush_udp_outbound_packets(const std::string &client_ip, std::size_t max_packets);

private:
    RtspResponse handle_request_with_context(
        const RtspRequest &request,
        const std::string &client_ip,
        uint64_t request_time_ms,
        uint64_t connection_id);

    coroutine::Task<void> handle_connection(AsyncConnectionContext ctx);

    void collect_expired_sessions_locked(uint64_t current_ms);
    void record_audit_event_locked(RtspAuditEvent event);
    bool queue_interleaved_report_locked(const std::string &session_id, const std::string &track_uri, bool sender_report);
    bool queue_udp_report_locked(const std::string &session_id, const std::string &track_uri, bool sender_report);
    bool maybe_queue_auto_feedback_locked(const std::string &session_id, const std::string &track_uri, bool frame_is_rtcp, uint64_t now_ms);

private:
    struct SessionMediaBridge
    {
        SessionMediaBridge()
            : rtp_manager(90000), rtcp_session(0)
        {
        }

        ::yuan::net::rtc::RtpSessionManager rtp_manager;
        ::yuan::net::rtcp::RtcpSession rtcp_session;
    };

private:
    int port_ = 0;
    uint64_t default_session_timeout_ms_ = 60000;
    AsyncListenerHost listener_;
    std::unique_ptr<NetworkRuntime> owned_runtime_;
    RequestHandler handler_;
    std::string basic_auth_realm_;
    std::string basic_auth_username_;
    std::string basic_auth_password_;
    bool basic_auth_enabled_ = false;
    std::string digest_auth_realm_;
    std::string digest_auth_username_;
    std::string digest_auth_password_;
    bool digest_auth_enabled_ = false;

    RtspAclConfig acl_config_;
    RtspRateLimitConfig rate_limit_config_;

    struct NonceEntry
    {
        uint64_t issued_ms = 0;
        uint64_t expire_ms = 0;
        std::string opaque;
    };

    struct ClientRateState
    {
        uint64_t window_start_ms = 0;
        uint32_t request_count = 0;
        uint32_t auth_failures = 0;
        uint64_t ban_until_ms = 0;
    };

    std::map<std::string, NonceEntry> digest_nonces_;
    std::map<std::string, uint64_t> digest_nonce_nc_watermark_;
    std::map<std::string, ClientRateState> client_rate_states_;
    SecurityStatsSnapshot security_stats_;
    RtspMetricsSnapshot metrics_;
    RtspObservabilityConfig observability_config_;
    std::deque<RtspAuditEvent> audit_events_;
    mutable std::mutex sessions_mutex_;
    std::map<std::string, RtspSession> sessions_;
    std::map<std::string, uint64_t> expired_sessions_;
    std::map<std::string, SessionMediaBridge> media_bridges_;
    std::map<std::string, uint64_t> session_connection_owner_;
    InterleavedStatsSnapshot interleaved_stats_;
    std::deque<RtspOutboundPacket> outbound_packets_;
    std::map<std::string, uint64_t> feedback_rr_last_ms_;
    std::map<std::string, uint64_t> feedback_sr_last_ms_;
    uint64_t next_connection_id_ = 1;
};

} // namespace yuan::net::rtsp

#endif
