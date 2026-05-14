#ifndef __NET_WEBRTC_TRANSPORT_BRIDGE_H__
#define __NET_WEBRTC_TRANSPORT_BRIDGE_H__

#include "rtcp_session.h"
#include "rtp_session.h"
#include "webrtc_srtp.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace yuan::net::webrtc
{

struct RtcpScheduleConfig
{
    uint64_t report_interval_ms = 1000;
    uint32_t sender_report_every = 2;
    std::size_t rr_snapshot_reset_interval_reports = 0;
};

class WebrtcTransportBridge
{
public:
    explicit WebrtcTransportBridge(uint32_t local_ssrc = 0, uint32_t clock_rate = 90000);

    bool on_rtp_packet_received(const ::yuan::net::rtc::RtcPacket &packet, uint64_t arrival_time_ms);

    ::yuan::net::rtcp::RtcpPacket build_receiver_report() const;
    ::yuan::net::rtcp::RtcpPacket build_sender_report() const;

    void on_sender_activity(uint64_t ntp_timestamp, uint32_t rtp_timestamp, uint32_t packet_count, uint32_t octet_count);

    void set_srtp_context(std::shared_ptr<SrtpContext> context);
    bool has_srtp_context() const;
    std::shared_ptr<SrtpContext> srtp_context() const;
    bool on_srtp_rtp_packet_received(const ::yuan::net::rtc::RtcPacket &packet, uint64_t arrival_time_ms);
    bool protect_rtp_packet(::yuan::net::rtc::RtcPacket &packet) const;
    bool unprotect_rtp_packet(::yuan::net::rtc::RtcPacket &packet) const;
    bool protect_rtcp_packet(::yuan::net::rtcp::RtcpPacket &packet) const;
    bool unprotect_rtcp_packet(::yuan::net::rtcp::RtcpPacket &packet) const;

    void set_rtcp_schedule_config(const RtcpScheduleConfig &config);
    RtcpScheduleConfig rtcp_schedule_config() const;
    void reset_rtcp_schedule(uint64_t now_ms = 0);
    bool poll_scheduled_rtcp(uint64_t now_ms, ::yuan::net::rtcp::RtcpPacket &out_packet);

    ::yuan::net::rtc::RtpSessionManager &rtp_manager()
    {
        return rtp_manager_;
    }

    const ::yuan::net::rtc::RtpSessionManager &rtp_manager() const
    {
        return rtp_manager_;
    }

    ::yuan::net::rtcp::RtcpSession &rtcp_session()
    {
        return rtcp_session_;
    }

    const ::yuan::net::rtcp::RtcpSession &rtcp_session() const
    {
        return rtcp_session_;
    }

private:
    ::yuan::net::rtc::RtpSessionManager rtp_manager_;
    ::yuan::net::rtcp::RtcpSession rtcp_session_;
    RtcpScheduleConfig rtcp_schedule_config_;
    uint64_t next_report_due_ms_ = 0;
    uint64_t reports_emitted_ = 0;
    bool has_sender_activity_hint_ = false;
    std::shared_ptr<SrtpContext> srtp_context_;
};

} // namespace yuan::net::webrtc

#endif
