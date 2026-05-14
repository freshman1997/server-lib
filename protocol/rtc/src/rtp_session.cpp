#include "rtp_session.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace yuan::net::rtc
{

RtpSession::RtpSession(uint32_t ssrc, uint32_t clock_rate)
    : ssrc_(ssrc), has_ssrc_(ssrc != 0), clock_rate_(clock_rate == 0 ? 90000 : clock_rate)
{
}

void RtpSession::reset()
{
    initialized_ = false;
    transit_initialized_ = false;
    base_seq_ = 0;
    max_seq_ = 0;
    cycles_ = 0;
    received_ = 0;
    last_transit_ = 0;
    jitter_ = 0.0;
}

bool RtpSession::on_packet_received(const RtcPacket &packet, uint64_t arrival_time_ms)
{
    if (packet.version != 2) {
        return false;
    }

    if (!has_ssrc_) {
        ssrc_ = packet.ssrc;
        has_ssrc_ = true;
    } else if (packet.ssrc != ssrc_) {
        return false;
    }

    const uint16_t seq = packet.sequence_number;
    if (!initialized_) {
        initialized_ = true;
        base_seq_ = seq;
        max_seq_ = seq;
        cycles_ = 0;
    } else {
        if (seq < max_seq_ && static_cast<uint16_t>(max_seq_ - seq) > 0x8000u) {
            cycles_ += 0x10000u;
        }
        if (static_cast<uint16_t>(seq - max_seq_) < 0x8000u) {
            max_seq_ = seq;
        }
    }

    ++received_;

    const int64_t arrival_ts = static_cast<int64_t>((arrival_time_ms * clock_rate_) / 1000);
    const int64_t packet_ts = static_cast<int64_t>(packet.timestamp);
    const int64_t transit = arrival_ts - packet_ts;
    if (!transit_initialized_) {
        transit_initialized_ = true;
        last_transit_ = transit;
    } else {
        const int64_t d = transit - last_transit_;
        const double abs_d = static_cast<double>(d < 0 ? -d : d);
        jitter_ += (abs_d - jitter_) / 16.0;
        last_transit_ = transit;
    }

    return true;
}

RtpReceiveStats RtpSession::receive_stats() const
{
    RtpReceiveStats stats;
    stats.ssrc = ssrc_;
    stats.initialized = initialized_;

    if (!initialized_) {
        return stats;
    }

    const uint32_t ext_base = static_cast<uint32_t>(base_seq_);
    const uint32_t ext_max = cycles_ + static_cast<uint32_t>(max_seq_);
    const uint32_t expected = (ext_max >= ext_base) ? (ext_max - ext_base + 1) : 0;
    const int64_t lost = static_cast<int64_t>(expected) - static_cast<int64_t>(received_);

    stats.base_sequence_number = ext_base;
    stats.highest_sequence_number = ext_max;
    stats.packets_received = received_;
    stats.expected_packets = expected;
    stats.cumulative_lost = clamp_signed_24(lost);
    stats.jitter = static_cast<uint32_t>(std::lround(std::max(0.0, jitter_)));
    return stats;
}

int32_t RtpSession::clamp_signed_24(int64_t value)
{
    constexpr int64_t kMin = -0x800000;
    constexpr int64_t kMax = 0x7FFFFF;
    if (value < kMin) {
        return static_cast<int32_t>(kMin);
    }
    if (value > kMax) {
        return static_cast<int32_t>(kMax);
    }
    return static_cast<int32_t>(value);
}

RtpSessionManager::RtpSessionManager(uint32_t clock_rate)
    : clock_rate_(clock_rate == 0 ? 90000 : clock_rate)
{
}

bool RtpSessionManager::on_packet_received(const RtcPacket &packet, uint64_t arrival_time_ms)
{
    auto iter = sessions_.find(packet.ssrc);
    if (iter == sessions_.end()) {
        auto session = std::make_unique<RtpSession>(packet.ssrc, clock_rate_);
        if (!session->on_packet_received(packet, arrival_time_ms)) {
            ++rejected_packets_;
            return false;
        }
        sessions_.emplace(packet.ssrc, std::move(session));
        return true;
    }

    if (!iter->second->on_packet_received(packet, arrival_time_ms)) {
        if (conflict_policy_ == RtpSsrcConflictPolicy::replace) {
            auto replacement = std::make_unique<RtpSession>(packet.ssrc, clock_rate_);
            if (!replacement->on_packet_received(packet, arrival_time_ms)) {
                ++rejected_packets_;
                return false;
            }
            iter->second = std::move(replacement);
            return true;
        }
        ++rejected_packets_;
        return false;
    }

    return true;
}

bool RtpSessionManager::has_session(uint32_t ssrc) const
{
    return sessions_.find(ssrc) != sessions_.end();
}

RtpSession *RtpSessionManager::find_session(uint32_t ssrc)
{
    auto iter = sessions_.find(ssrc);
    if (iter == sessions_.end()) {
        return nullptr;
    }
    return iter->second.get();
}

const RtpSession *RtpSessionManager::find_session(uint32_t ssrc) const
{
    auto iter = sessions_.find(ssrc);
    if (iter == sessions_.end()) {
        return nullptr;
    }
    return iter->second.get();
}

bool RtpSessionManager::attach_session(uint32_t ssrc, std::unique_ptr<RtpSession> session)
{
    if (!session) {
        return false;
    }
    auto [_, inserted] = sessions_.emplace(ssrc, std::move(session));
    return inserted;
}

std::vector<uint32_t> RtpSessionManager::all_ssrcs() const
{
    std::vector<uint32_t> ids;
    ids.reserve(sessions_.size());
    for (const auto &entry : sessions_) {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace yuan::net::rtc
