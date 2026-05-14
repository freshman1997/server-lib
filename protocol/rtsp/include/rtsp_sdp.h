#ifndef __NET_RTSP_SDP_H__
#define __NET_RTSP_SDP_H__

#include <string>
#include <vector>

namespace yuan::net::rtsp
{

struct RtspSdpMedia
{
    std::string kind;
    int payload_type = -1;
    std::string codec;
    int clock_rate = 0;
};

struct RtspSdpDescription
{
    std::string session_name = "rtsp-stream";
    std::vector<RtspSdpMedia> media;
};

bool parse_sdp(const std::string &text, RtspSdpDescription &out_description);
bool serialize_sdp(const RtspSdpDescription &description, std::string &out_text);

} // namespace yuan::net::rtsp

#endif
