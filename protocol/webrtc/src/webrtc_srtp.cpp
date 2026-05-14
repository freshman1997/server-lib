#include "webrtc_srtp.h"

namespace yuan::net::webrtc
{

void MockSrtpContext::set_config(const MockSrtpConfig &config)
{
    config_ = config;
}

MockSrtpConfig MockSrtpContext::config() const
{
    return config_;
}

bool MockSrtpContext::protect_rtp(::yuan::net::rtc::RtcPacket &packet)
{
    (void) packet;
    ++protect_rtp_calls_;
    return !config_.fail_protect_rtp;
}

bool MockSrtpContext::unprotect_rtp(::yuan::net::rtc::RtcPacket &packet)
{
    (void) packet;
    ++unprotect_rtp_calls_;
    return !config_.fail_unprotect_rtp;
}

bool MockSrtpContext::protect_rtcp(::yuan::net::rtcp::RtcpPacket &packet)
{
    (void) packet;
    ++protect_rtcp_calls_;
    return !config_.fail_protect_rtcp;
}

bool MockSrtpContext::unprotect_rtcp(::yuan::net::rtcp::RtcpPacket &packet)
{
    (void) packet;
    ++unprotect_rtcp_calls_;
    return !config_.fail_unprotect_rtcp;
}

bool MockSrtpContext::apply_keying_material(const DtlsSrtpKeyingMaterial &material)
{
    keying_material_ = material;
    has_keying_material_ = material.profile != DtlsSrtpProfile::unknown
                           && !material.client_master_key.empty()
                           && !material.server_master_key.empty()
                           && !material.client_master_salt.empty()
                           && !material.server_master_salt.empty();
    return has_keying_material_;
}

bool MockSrtpContext::has_keying_material() const
{
    return has_keying_material_;
}

std::size_t MockSrtpContext::protect_rtp_calls() const
{
    return protect_rtp_calls_;
}

std::size_t MockSrtpContext::unprotect_rtp_calls() const
{
    return unprotect_rtp_calls_;
}

std::size_t MockSrtpContext::protect_rtcp_calls() const
{
    return protect_rtcp_calls_;
}

std::size_t MockSrtpContext::unprotect_rtcp_calls() const
{
    return unprotect_rtcp_calls_;
}

} // namespace yuan::net::webrtc
