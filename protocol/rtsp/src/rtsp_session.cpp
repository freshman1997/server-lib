#include "rtsp_session.h"

namespace yuan::net::rtsp
{

RtspSession::RtspSession(std::string id)
    : id_(std::move(id))
{
}

const std::string &RtspSession::id() const
{
    return id_;
}

RtspSessionState RtspSession::state() const
{
    return state_;
}

bool RtspSession::on_setup(const std::string &track_uri, const RtspTransportSpec &transport)
{
    if (state_ == RtspSessionState::closed) {
        return false;
    }
    if (track_uri.empty()) {
        return false;
    }
    transport_ = transport;
    has_transport_ = true;
    if (std::find(tracks_.begin(), tracks_.end(), track_uri) == tracks_.end()) {
        tracks_.push_back(track_uri);
    }
    bool updated_binding = false;
    for (auto &binding : track_bindings_) {
        if (binding.track_uri == track_uri) {
            binding.transport = transport;
            updated_binding = true;
            break;
        }
    }
    if (!updated_binding) {
        track_bindings_.push_back(TrackBinding{track_uri, transport});
    }
    state_ = RtspSessionState::ready;
    return true;
}

bool RtspSession::on_play()
{
    if (state_ != RtspSessionState::ready) {
        return false;
    }
    state_ = RtspSessionState::playing;
    return true;
}

bool RtspSession::on_pause()
{
    if (state_ != RtspSessionState::playing) {
        return false;
    }
    state_ = RtspSessionState::ready;
    return true;
}

bool RtspSession::on_announce(std::size_t media_count, const std::string &announce_fingerprint)
{
    if (state_ != RtspSessionState::ready) {
        return false;
    }
    if (tracks_.empty() || media_count == 0 || media_count != tracks_.size()) {
        return false;
    }

    if (announced_) {
        if (announced_media_count_ != media_count) {
            return false;
        }
        if (!announced_fingerprint_.empty() &&
            !announce_fingerprint.empty() &&
            announced_fingerprint_ != announce_fingerprint) {
            return false;
        }
    }

    announced_ = true;
    announced_media_count_ = media_count;
    announced_fingerprint_ = announce_fingerprint;
    return true;
}

bool RtspSession::on_record()
{
    if (state_ != RtspSessionState::ready) {
        return false;
    }
    if (!announced_) {
        return false;
    }
    state_ = RtspSessionState::recording;
    return true;
}

void RtspSession::on_teardown()
{
    has_transport_ = false;
    announced_ = false;
    announced_media_count_ = 0;
    announced_fingerprint_.clear();
    tracks_.clear();
    track_bindings_.clear();
    state_ = RtspSessionState::closed;
}

int RtspSession::last_cseq() const
{
    return last_cseq_;
}

bool RtspSession::validate_cseq(int cseq)
{
    if (cseq < 0) {
        return false;
    }
    if (last_cseq_ >= 0 && cseq <= last_cseq_) {
        return false;
    }
    last_cseq_ = cseq;
    return true;
}

void RtspSession::touch(uint64_t now_ms)
{
    last_active_ms_ = now_ms;
}

bool RtspSession::is_expired(uint64_t now_ms, uint64_t timeout_ms) const
{
    if (timeout_ms == 0) {
        return false;
    }
    if (last_active_ms_ == 0) {
        return false;
    }
    if (now_ms < last_active_ms_) {
        return false;
    }
    return (now_ms - last_active_ms_) > timeout_ms;
}

void RtspSession::set_timeout_ms(uint64_t timeout_ms)
{
    if (timeout_ms == 0) {
        timeout_ms_ = 60000;
        return;
    }
    timeout_ms_ = timeout_ms;
}

uint64_t RtspSession::timeout_ms() const
{
    return timeout_ms_;
}

bool RtspSession::has_transport() const
{
    return has_transport_;
}

const RtspTransportSpec &RtspSession::transport() const
{
    return transport_;
}

void RtspSession::set_announced(bool announced)
{
    announced_ = announced;
    if (!announced) {
        announced_media_count_ = 0;
        announced_fingerprint_.clear();
    }
}

bool RtspSession::is_announced() const
{
    return announced_;
}

bool RtspSession::has_track(const std::string &track_uri) const
{
    return std::find(tracks_.begin(), tracks_.end(), track_uri) != tracks_.end();
}

const std::vector<std::string> &RtspSession::tracks() const
{
    return tracks_;
}

bool RtspSession::has_interleaved_channel_conflict(const RtspTransportSpec &transport) const
{
    if (transport.transport != RtspLowerTransport::rtp_avp_tcp ||
        transport.interleaved_rtp_channel < 0 || transport.interleaved_rtcp_channel < 0) {
        return false;
    }

    for (const auto &binding : track_bindings_) {
        const RtspTransportSpec &existing = binding.transport;
        if (existing.transport != RtspLowerTransport::rtp_avp_tcp ||
            existing.interleaved_rtp_channel < 0 || existing.interleaved_rtcp_channel < 0) {
            continue;
        }
        if (transport.interleaved_rtp_channel == existing.interleaved_rtp_channel ||
            transport.interleaved_rtp_channel == existing.interleaved_rtcp_channel ||
            transport.interleaved_rtcp_channel == existing.interleaved_rtp_channel ||
            transport.interleaved_rtcp_channel == existing.interleaved_rtcp_channel) {
            return true;
        }
    }

    return false;
}

bool RtspSession::resolve_interleaved_channel(uint8_t channel, bool &out_is_rtcp) const
{
    for (const auto &binding : track_bindings_) {
        const RtspTransportSpec &transport = binding.transport;
        if (transport.transport != RtspLowerTransport::rtp_avp_tcp) {
            continue;
        }
        if (transport.interleaved_rtp_channel >= 0 &&
            static_cast<uint8_t>(transport.interleaved_rtp_channel) == channel) {
            out_is_rtcp = false;
            return true;
        }
        if (transport.interleaved_rtcp_channel >= 0 &&
            static_cast<uint8_t>(transport.interleaved_rtcp_channel) == channel) {
            out_is_rtcp = true;
            return true;
        }
    }
    return false;
}

bool RtspSession::resolve_track_interleaved_channels(
    const std::string &track_uri,
    uint8_t &out_rtp_channel,
    uint8_t &out_rtcp_channel) const
{
    for (const auto &binding : track_bindings_) {
        if (binding.track_uri != track_uri) {
            continue;
        }
        const RtspTransportSpec &transport = binding.transport;
        if (transport.transport != RtspLowerTransport::rtp_avp_tcp) {
            return false;
        }
        if (transport.interleaved_rtp_channel < 0 || transport.interleaved_rtcp_channel < 0) {
            return false;
        }
        out_rtp_channel = static_cast<uint8_t>(transport.interleaved_rtp_channel);
        out_rtcp_channel = static_cast<uint8_t>(transport.interleaved_rtcp_channel);
        return true;
    }
    return false;
}

bool RtspSession::resolve_track_by_interleaved_channel(uint8_t channel, std::string &out_track_uri, bool &out_is_rtcp) const
{
    for (const auto &binding : track_bindings_) {
        const RtspTransportSpec &transport = binding.transport;
        if (transport.transport != RtspLowerTransport::rtp_avp_tcp) {
            continue;
        }
        if (transport.interleaved_rtp_channel >= 0 &&
            static_cast<uint8_t>(transport.interleaved_rtp_channel) == channel) {
            out_track_uri = binding.track_uri;
            out_is_rtcp = false;
            return true;
        }
        if (transport.interleaved_rtcp_channel >= 0 &&
            static_cast<uint8_t>(transport.interleaved_rtcp_channel) == channel) {
            out_track_uri = binding.track_uri;
            out_is_rtcp = true;
            return true;
        }
    }
    return false;
}

bool RtspSession::resolve_track_transport(const std::string &track_uri, RtspTransportSpec &out_transport) const
{
    for (const auto &binding : track_bindings_) {
        if (binding.track_uri == track_uri) {
            out_transport = binding.transport;
            return true;
        }
    }
    return false;
}

} // namespace yuan::net::rtsp
