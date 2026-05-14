#ifndef __NET_WEBRTC_SRTP_H__
#define __NET_WEBRTC_SRTP_H__

#include "webrtc_dtls_transport.h"
#include "rtcp_packet.h"
#include "rtc_packet.h"

#include <cstddef>

namespace yuan::net::webrtc
{

class SrtpContext
{
public:
    virtual ~SrtpContext() = default;

    virtual bool protect_rtp(::yuan::net::rtc::RtcPacket &packet) = 0;
    virtual bool unprotect_rtp(::yuan::net::rtc::RtcPacket &packet) = 0;
    virtual bool protect_rtcp(::yuan::net::rtcp::RtcpPacket &packet) = 0;
    virtual bool unprotect_rtcp(::yuan::net::rtcp::RtcpPacket &packet) = 0;
    virtual bool apply_keying_material(const DtlsSrtpKeyingMaterial &material) = 0;
    virtual bool has_keying_material() const = 0;
};

struct MockSrtpConfig
{
    bool fail_protect_rtp = false;
    bool fail_unprotect_rtp = false;
    bool fail_protect_rtcp = false;
    bool fail_unprotect_rtcp = false;
};

class MockSrtpContext : public SrtpContext
{
public:
    void set_config(const MockSrtpConfig &config);
    MockSrtpConfig config() const;

    bool protect_rtp(::yuan::net::rtc::RtcPacket &packet) override;
    bool unprotect_rtp(::yuan::net::rtc::RtcPacket &packet) override;
    bool protect_rtcp(::yuan::net::rtcp::RtcpPacket &packet) override;
    bool unprotect_rtcp(::yuan::net::rtcp::RtcpPacket &packet) override;
    bool apply_keying_material(const DtlsSrtpKeyingMaterial &material) override;
    bool has_keying_material() const override;

    std::size_t protect_rtp_calls() const;
    std::size_t unprotect_rtp_calls() const;
    std::size_t protect_rtcp_calls() const;
    std::size_t unprotect_rtcp_calls() const;

private:
    MockSrtpConfig config_;
    std::size_t protect_rtp_calls_ = 0;
    std::size_t unprotect_rtp_calls_ = 0;
    std::size_t protect_rtcp_calls_ = 0;
    std::size_t unprotect_rtcp_calls_ = 0;
    bool has_keying_material_ = false;
    DtlsSrtpKeyingMaterial keying_material_;
};

} // namespace yuan::net::webrtc

#endif
