#include "rtcp_session.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace
{

struct ReportEntry
{
    ::yuan::net::rtc::RtpReceiveStats stats;
    uint8_t fraction_lost = 0;
    std::size_t input_index = 0;
};

uint32_t ntp64_middle_32(uint64_t ntp)
{
    return static_cast<uint32_t>((ntp >> 16) & 0xFFFFFFFFull);
}

uint64_t steady_now_ms()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now().time_since_epoch()).count());
}

uint32_t calc_delay_65536(uint64_t now_ms, uint64_t then_ms)
{
    if (now_ms <= then_ms) {
        return 0;
    }
    const uint64_t diff_ms = now_ms - then_ms;
    const uint64_t units = (diff_ms * 65536ull) / 1000ull;
    if (units > 0xFFFFFFFFull) {
        return 0xFFFFFFFFu;
    }
    return static_cast<uint32_t>(units);
}

int32_t clamp_signed_24(int64_t value)
{
    if (value < -0x800000) {
        return -0x800000;
    }
    if (value > 0x7FFFFF) {
        return 0x7FFFFF;
    }
    return static_cast<int32_t>(value);
}

uint8_t calc_fraction_lost_interval(
    const ::yuan::net::rtc::RtpReceiveStats &stats,
    bool has_snapshot,
    uint32_t previous_expected_packets,
    uint32_t previous_received_packets)
{
    if (!has_snapshot) {
        return 0;
    }

    const int64_t expected_interval =
        static_cast<int64_t>(stats.expected_packets) - static_cast<int64_t>(previous_expected_packets);
    const int64_t received_interval =
        static_cast<int64_t>(stats.packets_received) - static_cast<int64_t>(previous_received_packets);
    const int64_t lost_interval = expected_interval - received_interval;

    if (expected_interval <= 0 || lost_interval <= 0) {
        return 0;
    }

    int64_t scaled = (lost_interval * 256) / expected_interval;
    scaled = std::clamp<int64_t>(scaled, 0, 255);
    return static_cast<uint8_t>(scaled);
}

::yuan::net::rtcp::RtcpReportBlock make_report_block(
    const ::yuan::net::rtc::RtpReceiveStats &stats,
    uint8_t fraction_lost)
{
    ::yuan::net::rtcp::RtcpReportBlock block;
    block.ssrc = stats.ssrc;
    const int64_t expected = static_cast<int64_t>(stats.expected_packets);
    const int64_t received = static_cast<int64_t>(stats.packets_received);
    block.cumulative_lost = clamp_signed_24(expected - received);
    block.fraction_lost = fraction_lost;
    block.highest_sequence_number = stats.highest_sequence_number;
    block.jitter = stats.jitter;
    block.last_sr = 0;
    block.delay_since_last_sr = 0;
    return block;
}

} // namespace

namespace yuan::net::rtcp
{

namespace
{

std::vector< ::yuan::net::rtc::RtpReceiveStats> collect_stats_from_manager(const ::yuan::net::rtc::RtpSessionManager &manager)
{
    std::vector< ::yuan::net::rtc::RtpReceiveStats> stats_list;
    const auto ssrcs = manager.all_ssrcs();
    stats_list.reserve(ssrcs.size());
    for (uint32_t ssrc : ssrcs) {
        const auto *session = manager.find_session(ssrc);
        if (!session) {
            continue;
        }
        stats_list.push_back(session->receive_stats());
    }
    return stats_list;
}

std::vector<ReportEntry> build_report_entries(
    const std::vector< ::yuan::net::rtc::RtpReceiveStats> &stats_list,
    const std::unordered_map<uint32_t, RtcpSession::RrSnapshot> &snapshots,
    RtcpReportOrderPolicy order_policy)
{
    std::vector<ReportEntry> entries;
    entries.reserve(stats_list.size());

    for (std::size_t index = 0; index < stats_list.size(); ++index) {
        const auto &stats = stats_list[index];
        if (!stats.initialized) {
            continue;
        }
        const auto iter = snapshots.find(stats.ssrc);
        const bool has_snapshot = iter != snapshots.end();
        const uint32_t prev_expected = has_snapshot ? iter->second.expected_packets : 0;
        const uint32_t prev_received = has_snapshot ? iter->second.received_packets : 0;
        entries.push_back(ReportEntry{
            stats,
            calc_fraction_lost_interval(stats, has_snapshot, prev_expected, prev_received),
            index});
    }

    if (order_policy == RtcpReportOrderPolicy::ssrc_ascending) {
        std::sort(entries.begin(), entries.end(), [](const ReportEntry &a, const ReportEntry &b) {
            if (a.stats.ssrc != b.stats.ssrc) {
                return a.stats.ssrc < b.stats.ssrc;
            }
            return a.input_index < b.input_index;
        });
    }

    return entries;
}

} // namespace

void RtcpSession::maybe_reset_rr_snapshots_before_build() const
{
    if (snapshot_reset_interval_ == 0) {
        return;
    }

    if (rr_reports_built_ >= snapshot_reset_interval_) {
        rr_snapshots_.clear();
        rr_reports_built_ = 0;
    }
}

void RtcpSession::mark_rr_report_built() const
{
    if (snapshot_reset_interval_ == 0) {
        ++rr_reports_total_;
        return;
    }

    ++rr_reports_built_;
    ++rr_reports_total_;
}

void RtcpSession::mark_sr_report_built() const
{
    ++sr_reports_total_;
}

RtcpSession::RtcpSession(uint32_t local_ssrc)
    : local_ssrc_(local_ssrc)
{
}

void RtcpSession::on_sender_activity(
    uint64_t ntp_timestamp,
    uint32_t rtp_timestamp,
    uint32_t packet_count,
    uint32_t octet_count,
    uint64_t arrival_time_ms)
{
    has_sender_activity_ = true;
    sender_ntp_timestamp_ = ntp_timestamp;
    sender_rtp_timestamp_ = rtp_timestamp;
    sender_packet_count_ = packet_count;
    sender_octet_count_ = octet_count;
    if (arrival_time_ms > 0) {
        sender_activity_received_ms_ = arrival_time_ms;
    }
    last_sr_lsr_ = ntp64_middle_32(ntp_timestamp);
}

RtcpPacket RtcpSession::build_receiver_report(const ::yuan::net::rtc::RtpReceiveStats &stats) const
{
    if (has_sender_activity_ && sender_activity_received_ms_ > 0) {
        last_sr_delay_65536_ = calc_delay_65536(steady_now_ms(), sender_activity_received_ms_);
    } else {
        last_sr_delay_65536_ = 0;
    }

    maybe_reset_rr_snapshots_before_build();

    RtcpPacket packet;
    packet.kind = RtcpPacket::Kind::receiver_report;
    packet.receiver_report.ssrc = local_ssrc_;

    const auto iter = rr_snapshots_.find(stats.ssrc);
    const bool has_snapshot = iter != rr_snapshots_.end();
    const uint32_t prev_expected = has_snapshot ? iter->second.expected_packets : 0;
    const uint32_t prev_received = has_snapshot ? iter->second.received_packets : 0;
    const uint8_t fraction_lost = calc_fraction_lost_interval(stats, has_snapshot, prev_expected, prev_received);

    auto block = make_report_block(stats, fraction_lost);
    block.last_sr = last_sr_lsr_;
    block.delay_since_last_sr = last_sr_delay_65536_;
    packet.receiver_report.report_blocks.push_back(block);
    rr_snapshots_[stats.ssrc] = RrSnapshot{stats.expected_packets, stats.packets_received};
    mark_rr_report_built();
    return packet;
}

RtcpPacket RtcpSession::build_receiver_report(const std::vector< ::yuan::net::rtc::RtpReceiveStats> &stats_list) const
{
    if (has_sender_activity_ && sender_activity_received_ms_ > 0) {
        last_sr_delay_65536_ = calc_delay_65536(steady_now_ms(), sender_activity_received_ms_);
    } else {
        last_sr_delay_65536_ = 0;
    }

    maybe_reset_rr_snapshots_before_build();

    RtcpPacket packet;
    packet.kind = RtcpPacket::Kind::receiver_report;
    packet.receiver_report.ssrc = local_ssrc_;

    const auto entries = build_report_entries(stats_list, rr_snapshots_, report_order_policy_);
    for (const auto &entry : entries) {
        auto block = make_report_block(entry.stats, entry.fraction_lost);
        block.last_sr = last_sr_lsr_;
        block.delay_since_last_sr = last_sr_delay_65536_;
        packet.receiver_report.report_blocks.push_back(block);
        rr_snapshots_[entry.stats.ssrc] = RrSnapshot{entry.stats.expected_packets, entry.stats.packets_received};
    }

    if (packet.receiver_report.report_blocks.size() > max_report_blocks_) {
        packet.receiver_report.report_blocks.resize(max_report_blocks_);
    }

    mark_rr_report_built();
    return packet;
}

RtcpPacket RtcpSession::build_sender_report(const ::yuan::net::rtc::RtpReceiveStats &stats) const
{
    RtcpPacket packet;
    packet.kind = RtcpPacket::Kind::sender_report;
    packet.sender_report.ssrc = local_ssrc_;
    packet.sender_report.ntp_timestamp = sender_ntp_timestamp_;
    packet.sender_report.rtp_timestamp = sender_rtp_timestamp_;
    packet.sender_report.packet_count = sender_packet_count_;
    packet.sender_report.octet_count = sender_octet_count_;
    packet.sender_report.report_blocks.push_back(make_report_block(stats, 0));

    if (!has_sender_activity_) {
        packet.sender_report.ntp_timestamp = 0;
        packet.sender_report.rtp_timestamp = 0;
        packet.sender_report.packet_count = 0;
        packet.sender_report.octet_count = 0;
    }
    mark_sr_report_built();
    return packet;
}

RtcpPacket RtcpSession::build_sender_report(const std::vector< ::yuan::net::rtc::RtpReceiveStats> &stats_list) const
{
    RtcpPacket packet;
    packet.kind = RtcpPacket::Kind::sender_report;
    packet.sender_report.ssrc = local_ssrc_;
    packet.sender_report.ntp_timestamp = sender_ntp_timestamp_;
    packet.sender_report.rtp_timestamp = sender_rtp_timestamp_;
    packet.sender_report.packet_count = sender_packet_count_;
    packet.sender_report.octet_count = sender_octet_count_;

    std::vector<ReportEntry> ordered_entries;
    ordered_entries.reserve(stats_list.size());
    for (std::size_t index = 0; index < stats_list.size(); ++index) {
        const auto &stats = stats_list[index];
        if (!stats.initialized) {
            continue;
        }
        ordered_entries.push_back(ReportEntry{stats, 0, index});
    }

    if (report_order_policy_ == RtcpReportOrderPolicy::ssrc_ascending) {
        std::sort(ordered_entries.begin(), ordered_entries.end(), [](const ReportEntry &a, const ReportEntry &b) {
            if (a.stats.ssrc != b.stats.ssrc) {
                return a.stats.ssrc < b.stats.ssrc;
            }
            return a.input_index < b.input_index;
        });
    }

    for (const auto &entry : ordered_entries) {
        packet.sender_report.report_blocks.push_back(make_report_block(entry.stats, 0));
    }

    if (packet.sender_report.report_blocks.size() > max_report_blocks_) {
        packet.sender_report.report_blocks.resize(max_report_blocks_);
    }

    if (!has_sender_activity_) {
        packet.sender_report.ntp_timestamp = 0;
        packet.sender_report.rtp_timestamp = 0;
        packet.sender_report.packet_count = 0;
        packet.sender_report.octet_count = 0;
    }

    mark_sr_report_built();
    return packet;
}

RtcpPacket RtcpSession::build_receiver_report(const ::yuan::net::rtc::RtpSessionManager &manager) const
{
    return build_receiver_report(collect_stats_from_manager(manager));
}

RtcpPacket RtcpSession::build_sender_report(const ::yuan::net::rtc::RtpSessionManager &manager) const
{
    return build_sender_report(collect_stats_from_manager(manager));
}

RtcpSession::StatsSnapshot RtcpSession::stats_snapshot() const
{
    StatsSnapshot out;
    out.has_sender_activity = has_sender_activity_;
    out.sender_ntp_timestamp = sender_ntp_timestamp_;
    out.sender_rtp_timestamp = sender_rtp_timestamp_;
    out.sender_packet_count = sender_packet_count_;
    out.sender_octet_count = sender_octet_count_;
    out.rr_reports_built = rr_reports_total_;
    out.sr_reports_built = sr_reports_total_;
    out.last_sr_lsr = last_sr_lsr_;
    out.last_sr_delay_65536 = last_sr_delay_65536_;
    return out;
}

} // namespace yuan::net::rtcp
