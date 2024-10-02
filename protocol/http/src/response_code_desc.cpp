#include "response_code_desc.h"

namespace net::http 
{
    std::unordered_map<ResponseCode, std::string> responseCodeDescs = {
        {ResponseCode::ok_, "200 OK"},
        {ResponseCode::bad_request, "400 Bad Request"},
        {ResponseCode::partial_content, "206 Partial Content"},
        {ResponseCode::not_found, "404 Not Found"},
        {ResponseCode::internal_server_error, "500 Internal Server Error"},
        {ResponseCode::gateway_timeout, "504 Gateway Timeout"},
        {ResponseCode::bad_gateway, "502 Bad Gateway"},
        {ResponseCode::forbidden, "403 Forbidden"},
    };
}