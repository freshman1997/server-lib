#include "net/http/request_context.h"
#include "net/http/request.h"
#include "net/http/response.h"
#include <net/connection/connection.h>

namespace net::http 
{
    HttpRequestContext::HttpRequestContext(Connection *conn) : conn_(conn)
    {
        request_ = new HttpRequest(this);
        response_ = new HttpResponse(this);
    }

    HttpRequestContext::~HttpRequestContext()
    {
        delete request_;
        delete response_;
    }

    bool HttpRequestContext::parse()
    {
        if (!conn_) {
            return false;
        }

        return request_->parse_header(*conn_->get_input_buff());
    }

    void HttpRequestContext::send()
    {
        response_->send();
    }
}