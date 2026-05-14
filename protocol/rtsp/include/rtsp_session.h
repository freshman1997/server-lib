#ifndef __NET_RTSP_SESSION_H__
#define __NET_RTSP_SESSION_H__

#include "rtsp_transport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::rtsp
{

enum class RtspSessionState
{
    idle,
    ready,
    playing,
    recording,
    closed,
};

class RtspSession
{
public:
    explicit RtspSession(std::string id = {});

    const std::string &id() const;
    RtspSessionState state() const;

    bool on_setup(const std::string &track_uri, const RtspTransportSpec &transport);
    bool on_play();
    bool on_pause();
    bool on_announce(std::size_t media_count, const std::string &announce_fingerprint);
    bool on_record();
    void on_teardown();

    int last_cseq() const;
    bool validate_cseq(int cseq);
    void touch(uint64_t now_ms);
    bool is_expired(uint64_t now_ms, uint64_t timeout_ms) const;
    void set_timeout_ms(uint64_t timeout_ms);
    uint64_t timeout_ms() const;

    bool has_transport() const;
    const RtspTransportSpec &transport() const;

    void set_announced(bool announced);
    bool is_announced() const;

    bool has_track(const std::string &track_uri) const;
    const std::vector<std::string> &tracks() const;
    bool has_interleaved_channel_conflict(const RtspTransportSpec &transport) const;
    bool resolve_interleaved_channel(uint8_t channel, bool &out_is_rtcp) const;
    bool resolve_track_interleaved_channels(const std::string &track_uri, uint8_t &out_rtp_channel, uint8_t &out_rtcp_channel) const;
    bool resolve_track_by_interleaved_channel(uint8_t channel, std::string &out_track_uri, bool &out_is_rtcp) const;
    bool resolve_track_transport(const std::string &track_uri, RtspTransportSpec &out_transport) const;

private:
    struct TrackBinding
    {
        std::string track_uri;
        RtspTransportSpec transport;
    };

private:
    std::string id_;
    RtspSessionState state_ = RtspSessionState::idle;
    int last_cseq_ = -1;
    uint64_t last_active_ms_ = 0;
    uint64_t timeout_ms_ = 60000;
    bool has_transport_ = false;
    bool announced_ = false;
    std::size_t announced_media_count_ = 0;
    std::string announced_fingerprint_;
    RtspTransportSpec transport_;
    std::vector<std::string> tracks_;
    std::vector<TrackBinding> track_bindings_;
};

} // namespace yuan::net::rtsp

#endif
