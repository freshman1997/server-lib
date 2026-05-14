#ifndef __NET_WEBRTC_SDP_H__
#define __NET_WEBRTC_SDP_H__

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::webrtc
{

enum class SdpMediaDirection
{
    sendrecv,
    sendonly,
    recvonly,
    inactive,
};

struct SdpRtpMap
{
    int32_t payload_type = -1;
    std::string codec;
    int32_t clock_rate = 0;
    int32_t channels = 1;
};

struct SdpMediaSection
{
    std::string kind;
    uint16_t port = 9;
    std::string protocol;
    std::string mid;
    SdpMediaDirection direction = SdpMediaDirection::sendrecv;
    std::vector<int32_t> payload_types;
    std::vector<SdpRtpMap> rtp_maps;
};

struct SdpSession
{
    struct Fingerprint
    {
        std::string algorithm;
        std::string value;
    };

    std::string origin = "- 0 0 IN IP4 127.0.0.1";
    std::string session_name = "-";
    std::string timing = "0 0";
    std::string bundle_group;
    bool has_fingerprint = false;
    Fingerprint fingerprint;
    std::vector<SdpMediaSection> media_sections;
};

class WebrtcSdp
{
public:
    static bool parse(const std::string &sdp_text, SdpSession &out_session);
    static bool serialize(const SdpSession &session, std::string &out_sdp_text);
};

} // namespace yuan::net::webrtc

#endif
