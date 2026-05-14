#include "webrtc_transport_bridge.h"

#include <algorithm>
#include <utility>

namespace yuan::net::webrtc
{

WebrtcTransportBridge::WebrtcTransportBridge(uint32_t local_ssrc, uint32_t clock_rate)
    : rtp_manager_(clock_rate), rtcp_session_(local_ssrc)
{
}

bool WebrtcTransportBridge::on_rtp_packet_received(const ::yuan::net::rtc::RtcPacket &packet, uint64_t arrival_time_ms)
{
    return rtp_manager_.on_packet_received(packet, arrival_time_ms);
}

::yuan::net::rtcp::RtcpPacket WebrtcTransportBridge::build_receiver_report() const
{
    return rtcp_session_.build_receiver_report(rtp_manager_);
}

::yuan::net::rtcp::RtcpPacket WebrtcTransportBridge::build_sender_report() const
{
    return rtcp_session_.build_sender_report(rtp_manager_);
}

void WebrtcTransportBridge::on_sender_activity(
    uint64_t ntp_timestamp,
    uint32_t rtp_timestamp,
    uint32_t packet_count,
    uint32_t octet_count)
{
    rtcp_session_.on_sender_activity(ntp_timestamp, rtp_timestamp, packet_count, octet_count);
    has_sender_activity_hint_ = true;
}

void WebrtcTransportBridge::set_srtp_context(std::shared_ptr<SrtpContext> context)
{
    srtp_context_ = std::move(context);
}

bool WebrtcTransportBridge::has_srtp_context() const
{
    return static_cast<bool>(srtp_context_);
}

std::shared_ptr<SrtpContext> WebrtcTransportBridge::srtp_context() const
{
    return srtp_context_;
}

bool WebrtcTransportBridge::on_srtp_rtp_packet_received(const ::yuan::net::rtc::RtcPacket &packet, uint64_t arrival_time_ms)
{
    ::yuan::net::rtc::RtcPacket plain = packet;
    if (!unprotect_rtp_packet(plain)) {
        return false;
    }
    return on_rtp_packet_received(plain, arrival_time_ms);
}

bool WebrtcTransportBridge::protect_rtp_packet(::yuan::net::rtc::RtcPacket &packet) const
{
    if (!srtp_context_) {
        return true;
    }
    return srtp_context_->protect_rtp(packet);
}

bool WebrtcTransportBridge::unprotect_rtp_packet(::yuan::net::rtc::RtcPacket &packet) const
{
    if (!srtp_context_) {
        return true;
    }
    return srtp_context_->unprotect_rtp(packet);
}

bool WebrtcTransportBridge::protect_rtcp_packet(::yuan::net::rtcp::RtcpPacket &packet) const
{
    if (!srtp_context_) {
        return true;
    }
    return srtp_context_->protect_rtcp(packet);
}

bool WebrtcTransportBridge::unprotect_rtcp_packet(::yuan::net::rtcp::RtcpPacket &packet) const
{
    if (!srtp_context_) {
        return true;
    }
    return srtp_context_->unprotect_rtcp(packet);
}

void WebrtcTransportBridge::set_rtcp_schedule_config(const RtcpScheduleConfig &config)
{
    rtcp_schedule_config_.report_interval_ms = std::max<uint64_t>(1, config.report_interval_ms);
    rtcp_schedule_config_.sender_report_every = std::max<uint32_t>(1, config.sender_report_every);
    rtcp_schedule_config_.rr_snapshot_reset_interval_reports = config.rr_snapshot_reset_interval_reports;
    rtcp_session_.set_snapshot_reset_interval(rtcp_schedule_config_.rr_snapshot_reset_interval_reports);
}

RtcpScheduleConfig WebrtcTransportBridge::rtcp_schedule_config() const
{
    return rtcp_schedule_config_;
}

void WebrtcTransportBridge::reset_rtcp_schedule(uint64_t now_ms)
{
    next_report_due_ms_ = now_ms + rtcp_schedule_config_.report_interval_ms;
    reports_emitted_ = 0;
}

bool WebrtcTransportBridge::poll_scheduled_rtcp(uint64_t now_ms, ::yuan::net::rtcp::RtcpPacket &out_packet)
{
    if (next_report_due_ms_ == 0) {
        reset_rtcp_schedule(now_ms);
    }

    if (now_ms < next_report_due_ms_) {
        return false;
    }

    const bool emit_sender_report = has_sender_activity_hint_ && ((reports_emitted_ % rtcp_schedule_config_.sender_report_every) == 0);
    out_packet = emit_sender_report ? build_sender_report() : build_receiver_report();
    if (!protect_rtcp_packet(out_packet)) {
        return false;
    }
    ++reports_emitted_;

    do {
        next_report_due_ms_ += rtcp_schedule_config_.report_interval_ms;
    } while (next_report_due_ms_ <= now_ms);

    return true;
}

} // namespace yuan::net::webrtc
