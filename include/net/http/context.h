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
    class HttpPacket;

    enum class Mode
    {
        server,
        client
    };

    class HttpSessionContext
    {
    public:
        HttpSessionContext(net::Connection *conn_);
        ~HttpSessionContext();

        HttpRequest * get_request()
        {
            return request_;
        }

        HttpResponse * get_response()
        {
            return response_;
        }

        net::Connection * get_connection()
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

        ResponseCode get_error_code();

        bool try_parse_request_content();

        void process_error(ResponseCode errorCode = ResponseCode::internal_server_error);

        void set_mode(Mode mode)
        {
            mode_ = mode;
        }
        
        inline HttpPacket * get_packet();
        
    private:
        void reset();
        
    private:
        Mode mode_;
        bool has_parsed_;
        Connection *conn_;
        HttpRequest *request_;
        HttpResponse *response_;
        HttpSession *session_;
    };
}

#endif
