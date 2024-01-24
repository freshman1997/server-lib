#include "net/http/request_context.h"
#include "net/http/request.h"
#include "net/http/response.h"
#include <net/connection/connection.h>

namespace net::http 
{
    HttpRequestContext::HttpRequestContext(Connection *conn) : conn_(conn)
    {
        request = new HttpRequest(this);
        response = new HttpResponse(this);
    }

    HttpRequestContext::~HttpRequestContext()
    {

    }

    bool HttpRequestContext::parse()
    {
        if (!conn_) {
            return false;
        }

        return request->parse_header(*conn_->get_input_buff());
    }
}