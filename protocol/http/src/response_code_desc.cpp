#include "response_code_desc.h"

namespace yuan::net::http 
{
    std::unordered_map<ResponseCode, std::string> responseCodeDescs = {
        {ResponseCode::switch_protocol, "101 Switching Protocols"},
        {ResponseCode::ok_, "200 OK"},
        {ResponseCode::not_modified, "304 Not Modified"},
        {ResponseCode::bad_request, "400 Bad Request"},
        {ResponseCode::partial_content, "206 Partial Content"},
        {ResponseCode::not_found, "404 Not Found"},
        {ResponseCode::http_version_not_supported, "505 HTTP Version Not Supported"},
        {ResponseCode::internal_server_error, "500 Internal Server Error"},
        {ResponseCode::gateway_timeout, "504 Gateway Timeout"},
        {ResponseCode::bad_gateway, "502 Bad Gateway"},
        {ResponseCode::forbidden, "403 Forbidden"},
    };
}
