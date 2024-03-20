#include "net/http/request_context.h"
#include "net/http/request.h"
#include "net/http/response.h"
#include "net/connection/connection.h"

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

    void HttpRequestContext::pre_request()
    {
        request_->reset();
        response_->reset();
    }

    bool HttpRequestContext::parse()
    {
        if (!conn_) {
            return false;
        }

        return request_->parse(*conn_->get_input_buff());
    }

    bool HttpRequestContext::is_completed()
    {
        if (!conn_ || !request_) {
            return false;
        }

        return request_->is_ok();
    }

    bool HttpRequestContext::has_error()
    {
        return !request_->good();
    }

    void HttpRequestContext::send()
    {
        response_->send();
    }
}