#include "net/http/response_code_desc.h"

namespace net::http 
{
    std::unordered_map<ResponseCode, std::string> responseCodeDescs = {
        {ResponseCode::ok_, "200 OK"},
        {ResponseCode::bad_request, "403 Bad Request"},
        {ResponseCode::partial_content, "206 Partial Content"},
        {ResponseCode::not_found, "404 Not Found"},
        {ResponseCode::internal_server_error, "500 Internal Server Error"},
        {ResponseCode::gateway_timeout, "504 Gateway timeout"},
    };
}