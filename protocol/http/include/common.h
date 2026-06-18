#ifndef __HTTP_COMMON_H__
#define __HTTP_COMMON_H__
#include <functional>
#include <string>

#include "coroutine/task.h"

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
    typedef std::function<yuan::coroutine::Task<void> (HttpRequest *req, HttpResponse *resp)> async_request_function;


    class HttpSession;

    typedef std::function<void (HttpSession *)> close_callback;
}

#endif
