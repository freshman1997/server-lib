#ifndef __NET_RTSP_FRAMING_H__
#define __NET_RTSP_FRAMING_H__

#include <cstdint>
#include <string>
#include <string_view>

namespace yuan::net::rtsp
{

enum class RtspFrameKind
{
    rtsp_request,
    interleaved,
};

struct RtspFrame
{
    RtspFrameKind kind = RtspFrameKind::rtsp_request;
    std::string data;
    uint8_t channel = 0;
};

enum class RtspFrameParseResult
{
    ok,
    need_more,
    malformed,
};

class RtspStreamFramer
{
public:
    void push(std::string_view bytes);
    RtspFrameParseResult pop(RtspFrame &out_frame);
    void clear();

private:
    std::string buffer_;
};

} // namespace yuan::net::rtsp

#endif
