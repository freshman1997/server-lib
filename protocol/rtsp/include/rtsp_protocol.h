#ifndef __NET_RTSP_PROTOCOL_H__
#define __NET_RTSP_PROTOCOL_H__

#include <cstdint>
#include <string>

namespace yuan::net::rtsp
{

enum class RtspMethod
{
    options,
    describe,
    setup,
    play,
    pause,
    teardown,
    get_parameter,
    set_parameter,
    announce,
    record,
    unknown,
};

enum class RtspStatusCode : uint16_t
{
    ok = 200,
    request_timeout = 408,
    bad_request = 400,
    unauthorized = 401,
    forbidden = 403,
    not_found = 404,
    too_many_requests = 429,
    method_not_allowed = 405,
    session_not_found = 454,
    method_not_valid_in_this_state = 455,
    parameter_not_understood = 451,
    unsupported_transport = 461,
    internal_server_error = 500,
};

inline constexpr const char *kRtspVersion = "RTSP/1.0";

RtspMethod parse_method(const std::string &method_text);
const char *method_to_string(RtspMethod method);
const char *status_code_reason(RtspStatusCode code);

} // namespace yuan::net::rtsp

#endif
