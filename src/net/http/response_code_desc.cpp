#include "net/http/response_code_desc.h"

namespace net::http 
{
    std::unordered_map<ResponseCode, std::string> responseCodeDescs = {
        {ResponseCode::ok_, "OK"},
        {ResponseCode::partial_content, "Partial Content"},
        {ResponseCode::not_found, "Not Found"},
    };
}