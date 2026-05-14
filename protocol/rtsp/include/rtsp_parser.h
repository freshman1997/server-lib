#ifndef __NET_RTSP_PARSER_H__
#define __NET_RTSP_PARSER_H__

#include "rtsp_request.h"
#include "rtsp_response.h"

#include <string>

namespace yuan::net::rtsp
{

class RtspParser
{
public:
    static bool parse_request(const std::string &raw, RtspRequest &out_request);
    static std::string serialize_response(const RtspResponse &response);
};

} // namespace yuan::net::rtsp

#endif
