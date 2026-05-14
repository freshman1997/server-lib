#ifndef __NET_RTSP_RESPONSE_H__
#define __NET_RTSP_RESPONSE_H__

#include "rtsp_protocol.h"
#include "rtsp_request.h"

#include <string>

namespace yuan::net::rtsp
{

struct RtspResponse
{
    std::string version = kRtspVersion;
    RtspStatusCode status = RtspStatusCode::ok;
    int cseq = -1;
    RtspHeaderMap headers;
    std::string body;
};

} // namespace yuan::net::rtsp

#endif
