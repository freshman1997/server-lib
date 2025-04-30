#ifndef __HTTP_REQUEST_CONTEXT_H__
#define __HTTP_REQUEST_CONTEXT_H__
#include "response_code.h"

namespace yuan::net 
{
    class Connection;
}

namespace yuan::net::http 
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

        void send() const;

    public:
        bool parse();

        bool is_completed();

        bool has_error() const;

        ResponseCode get_error_code() const;

        bool try_parse_request_content() const;

        void process_error(ResponseCode errorCode = ResponseCode::internal_server_error) const;

        void set_mode(Mode mode)
        {
            mode_ = mode;
        }
        
        inline HttpPacket * get_packet() const;

        void set_is_pro_large_block(bool flag)
        {
            is_process_large_block_ = flag;
        }

        bool is_process_large_block() const
        {
            return is_process_large_block_;
        }
        
    private:
        void reset() const;
        
    private:
        Mode mode_;
        bool has_parsed_;
        bool is_process_large_block_;
        Connection *conn_;
        HttpRequest *request_;
        HttpResponse *response_;
        HttpSession *session_;
    };
}

#endif
