#include "net/http/response_code_desc.h"

using net::http::response_code::ResponseCode;

std::unordered_map<ResponseCode, std::string> responseCodeDescs = {
    {ResponseCode::ok_, "OK"},
    {ResponseCode::partial_content, "Partial Content"},
    {ResponseCode::not_found, "Not Found"},
};