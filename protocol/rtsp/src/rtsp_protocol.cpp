#include "rtsp_protocol.h"

namespace yuan::net::rtsp
{

RtspMethod parse_method(const std::string &method_text)
{
    if (method_text == "OPTIONS") {
        return RtspMethod::options;
    }
    if (method_text == "DESCRIBE") {
        return RtspMethod::describe;
    }
    if (method_text == "SETUP") {
        return RtspMethod::setup;
    }
    if (method_text == "PLAY") {
        return RtspMethod::play;
    }
    if (method_text == "PAUSE") {
        return RtspMethod::pause;
    }
    if (method_text == "TEARDOWN") {
        return RtspMethod::teardown;
    }
    if (method_text == "GET_PARAMETER") {
        return RtspMethod::get_parameter;
    }
    if (method_text == "SET_PARAMETER") {
        return RtspMethod::set_parameter;
    }
    if (method_text == "ANNOUNCE") {
        return RtspMethod::announce;
    }
    if (method_text == "RECORD") {
        return RtspMethod::record;
    }
    return RtspMethod::unknown;
}

const char *method_to_string(RtspMethod method)
{
    switch (method) {
        case RtspMethod::options:
            return "OPTIONS";
        case RtspMethod::describe:
            return "DESCRIBE";
        case RtspMethod::setup:
            return "SETUP";
        case RtspMethod::play:
            return "PLAY";
        case RtspMethod::pause:
            return "PAUSE";
        case RtspMethod::teardown:
            return "TEARDOWN";
        case RtspMethod::get_parameter:
            return "GET_PARAMETER";
        case RtspMethod::set_parameter:
            return "SET_PARAMETER";
        case RtspMethod::announce:
            return "ANNOUNCE";
        case RtspMethod::record:
            return "RECORD";
        default:
            return "UNKNOWN";
    }
}

const char *status_code_reason(RtspStatusCode code)
{
    switch (code) {
        case RtspStatusCode::ok:
            return "OK";
        case RtspStatusCode::request_timeout:
            return "Request Timeout";
        case RtspStatusCode::bad_request:
            return "Bad Request";
        case RtspStatusCode::unauthorized:
            return "Unauthorized";
        case RtspStatusCode::forbidden:
            return "Forbidden";
        case RtspStatusCode::not_found:
            return "Not Found";
        case RtspStatusCode::too_many_requests:
            return "Too Many Requests";
        case RtspStatusCode::method_not_allowed:
            return "Method Not Allowed";
        case RtspStatusCode::session_not_found:
            return "Session Not Found";
        case RtspStatusCode::method_not_valid_in_this_state:
            return "Method Not Valid In This State";
        case RtspStatusCode::parameter_not_understood:
            return "Parameter Not Understood";
        case RtspStatusCode::unsupported_transport:
            return "Unsupported Transport";
        case RtspStatusCode::internal_server_error:
            return "Internal Server Error";
        default:
            return "Unknown";
    }
}

} // namespace yuan::net::rtsp
