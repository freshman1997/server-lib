#ifndef __HTTP_REQUEST_CONTEXT_H__
#define __HTTP_REQUEST_CONTEXT_H__

#include "net/http/response_code.h"
namespace net 
{
    class Connection;
}

namespace net::http 
{
    class HttpRequest;
    class HttpResponse;
    class HttpSession;

    class HttpRequestContext
    {
    public:
        HttpRequestContext(Connection *conn_);
        ~HttpRequestContext();

        void reset();

        HttpRequest * get_request()
        {
            return request_;
        }

        HttpResponse * get_response()
        {
            return response_;
        }

        Connection * get_connection()
        {
            return conn_;
        }

        void set_session(HttpSession *session)
        {
            session_ = session;
        }

        HttpSession * get_session()
        {
            return session_;
        }

        void send();

    public:
        bool parse();

        bool is_completed();

        bool has_error();

        ResponseCode get_error_code() const;

        bool try_parse_request_content();

        void process_error(ResponseCode errorCode = ResponseCode::internal_server_error);

    private:
        Connection *conn_;
        HttpRequest *request_;
        HttpResponse *response_;
        HttpSession *session_;
    };
}

#endif
