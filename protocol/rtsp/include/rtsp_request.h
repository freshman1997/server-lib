#ifndef __NET_RTSP_REQUEST_H__
#define __NET_RTSP_REQUEST_H__

#include "rtsp_protocol.h"

#include <string>
#include <unordered_map>

namespace yuan::net::rtsp
{

using RtspHeaderMap = std::unordered_map<std::string, std::string>;

struct RtspRequest
{
    RtspMethod method = RtspMethod::unknown;
    std::string uri;
    std::string version = kRtspVersion;
    int cseq = -1;
    RtspHeaderMap headers;
    std::string body;

    const std::string *header(const std::string &name) const;
};

} // namespace yuan::net::rtsp

#endif
