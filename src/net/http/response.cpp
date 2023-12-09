#include "net/http/response.h"

namespace net::http
{
    HttpResponse::HttpResponse(std::shared_ptr<Channel> channel) : channel_(channel)
    {
        
    }
}