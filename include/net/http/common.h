#ifndef __HTTP_COMMON_H__
#define __HTTP_COMMON_H__
#include <functional>
#include <memory>

#include "request_context.h"

namespace net::http 
{
    typedef std::function<void (std::shared_ptr<net::http::HttpRequestContext> ctx)> request_function;
}

#endif