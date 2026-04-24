#ifndef __NET_HTTP_PROXY_H__
#define __NET_HTTP_PROXY_H__

#include "proxy_api.h"

namespace yuan::net::http
{
    class HttpServer;

    std::unique_ptr<HttpProxyHandler> create_http_proxy_handler(HttpServer &server);
}

#endif
