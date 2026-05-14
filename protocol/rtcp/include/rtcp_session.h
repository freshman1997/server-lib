#ifndef __NET_RTCP_SESSION_H__
#define __NET_RTCP_SESSION_H__

#include "rtcp_packet.h"
#include "rtp_session.h"

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace yuan::net::rtcp
{

enum class RtcpReportOrderPolicy
{
    input_order,
    ssrc_ascending
};

class RtcpSession
{
public:
    struct RrSnapshot
    {
        uint32_t expected_packets = 0;
        uint32_t received_packets = 0;
    };

    struct StatsSnapshot
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

    explicit RtcpSession(uint32_t local_ssrc = 0);

    void set_local_ssrc(uint32_t ssrc)
    {
        local_ssrc_ = ssrc;
    }

    uint32_t local_ssrc() const
    {
        return local_ssrc_;
    }

    void set_max_report_blocks(std::size_t max_blocks)
    {
        max_report_blocks_ = max_blocks;
    }

    std::size_t max_report_blocks() const
    {
        return max_report_blocks_;
    }

    void set_report_order_policy(RtcpReportOrderPolicy policy)
    {
        report_order_policy_ = policy;
    }

    RtcpReportOrderPolicy report_order_policy() const
    {
        return report_order_policy_;
    }

    void set_snapshot_reset_interval(std::size_t interval)
    {
        snapshot_reset_interval_ = interval;
    }

    std::size_t snapshot_reset_interval() const
    {
        return snapshot_reset_interval_;
    }

    void on_sender_activity(
        uint64_t ntp_timestamp,
        uint32_t rtp_timestamp,
        uint32_t packet_count,
        uint32_t octet_count,
        uint64_t arrival_time_ms = 0);

    RtcpPacket build_receiver_report(const ::yuan::net::rtc::RtpReceiveStats &stats) const;
    RtcpPacket build_receiver_report(const std::vector< ::yuan::net::rtc::RtpReceiveStats> &stats_list) const;
    RtcpPacket build_sender_report(const ::yuan::net::rtc::RtpReceiveStats &stats) const;
    RtcpPacket build_sender_report(const std::vector< ::yuan::net::rtc::RtpReceiveStats> &stats_list) const;
    RtcpPacket build_receiver_report(const ::yuan::net::rtc::RtpSessionManager &manager) const;
    RtcpPacket build_sender_report(const ::yuan::net::rtc::RtpSessionManager &manager) const;
    StatsSnapshot stats_snapshot() const;

private:
    void maybe_reset_rr_snapshots_before_build() const;
    void mark_rr_report_built() const;
    void mark_sr_report_built() const;

    uint32_t local_ssrc_ = 0;
    bool has_sender_activity_ = false;
    uint64_t sender_ntp_timestamp_ = 0;
    uint32_t sender_rtp_timestamp_ = 0;
    uint32_t sender_packet_count_ = 0;
    uint32_t sender_octet_count_ = 0;
    uint64_t sender_activity_received_ms_ = 0;
    std::size_t max_report_blocks_ = 31;
    RtcpReportOrderPolicy report_order_policy_ = RtcpReportOrderPolicy::input_order;
    std::size_t snapshot_reset_interval_ = 0;

    mutable std::unordered_map<uint32_t, RrSnapshot> rr_snapshots_;
    mutable std::size_t rr_reports_built_ = 0;
    mutable uint64_t rr_reports_total_ = 0;
    mutable uint64_t sr_reports_total_ = 0;
    mutable uint32_t last_sr_lsr_ = 0;
    mutable uint32_t last_sr_delay_65536_ = 0;
};

} // namespace yuan::net::rtcp

#endif
