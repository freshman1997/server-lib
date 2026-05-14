#ifndef __NET_RTP_SESSION_H__
#define __NET_RTP_SESSION_H__

#include "rtc_packet.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace yuan::net::rtc
{

struct RtpReceiveStats
{
    uint32_t ssrc = 0;
    bool initialized = false;
    uint32_t base_sequence_number = 0;
    uint32_t highest_sequence_number = 0;
    uint32_t packets_received = 0;
    uint32_t expected_packets = 0;
    int32_t cumulative_lost = 0;
    uint32_t jitter = 0;
};

class RtpSession
{
public:
    explicit RtpSession(uint32_t ssrc = 0, uint32_t clock_rate = 90000);

    void reset();

    bool on_packet_received(const RtcPacket &packet, uint64_t arrival_time_ms);

    uint32_t ssrc() const
    {
        return ssrc_;
    }

    bool has_ssrc() const
    {
        return has_ssrc_;
    }

    uint32_t clock_rate() const
    {
        return clock_rate_;
    }

    RtpReceiveStats receive_stats() const;

private:
    static int32_t clamp_signed_24(int64_t value);

    uint32_t ssrc_ = 0;
    bool has_ssrc_ = false;
    uint32_t clock_rate_ = 90000;

    bool initialized_ = false;
    uint16_t base_seq_ = 0;
    uint16_t max_seq_ = 0;
    uint32_t cycles_ = 0;
    uint32_t received_ = 0;

    bool transit_initialized_ = false;
    int64_t last_transit_ = 0;
    double jitter_ = 0.0;
};

enum class RtpSsrcConflictPolicy
{
    reject,
    replace
};

class RtpSessionManager
{
public:
    explicit RtpSessionManager(uint32_t clock_rate = 90000);

    void set_conflict_policy(RtpSsrcConflictPolicy policy)
    {
        conflict_policy_ = policy;
    }

    RtpSsrcConflictPolicy conflict_policy() const
    {
        return conflict_policy_;
    }

    bool on_packet_received(const RtcPacket &packet, uint64_t arrival_time_ms);

    bool has_session(uint32_t ssrc) const;
    RtpSession *find_session(uint32_t ssrc);
    const RtpSession *find_session(uint32_t ssrc) const;
    bool attach_session(uint32_t ssrc, std::unique_ptr<RtpSession> session);

    std::vector<uint32_t> all_ssrcs() const;
    std::size_t session_count() const
    {
        return sessions_.size();
    }

    uint64_t rejected_packets() const
    {
        return rejected_packets_;
    }

private:
    std::unordered_map<uint32_t, std::unique_ptr<RtpSession> > sessions_;
    uint32_t clock_rate_ = 90000;
    RtpSsrcConflictPolicy conflict_policy_ = RtpSsrcConflictPolicy::reject;
    uint64_t rejected_packets_ = 0;
};

} // namespace yuan::net::rtc

#endif
