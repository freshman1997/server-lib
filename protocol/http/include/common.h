#ifndef __HTTP_COMMON_H__
#define __HTTP_COMMON_H__
#include <functional>
#include <string>

namespace yuan::net::http 
{
    class HttpRequest;
    class HttpResponse;

    struct AccessRule
    {
        bool allow = true;
        std::string value;
    };

    typedef std::function<void (HttpRequest *req, HttpResponse *resp)> request_function;


    class HttpSession;

    typedef std::function<void (HttpSession *)> close_callback;
}

#endif
