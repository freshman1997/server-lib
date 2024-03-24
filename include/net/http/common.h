#ifndef __HTTP_COMMON_H__
#define __HTTP_COMMON_H__
#include <functional>

namespace net::http 
{
    class HttpRequest;
    class HttpResponse;

    typedef std::function<void (HttpRequest *req, HttpResponse *resp)> request_function;
}

#endif