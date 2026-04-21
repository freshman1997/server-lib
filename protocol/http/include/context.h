#ifndef __HTTP_REQUEST_CONTEXT_H__
#define __HTTP_REQUEST_CONTEXT_H__
#include "response_code.h"
#include "buffer/byte_buffer.h"

#include <memory>

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

    enum class Mode {
        server,
        client
    };

    class HttpSessionContext
    {
    public:
        HttpSessionContext(net::Connection *conn_);
        HttpSessionContext(const std::shared_ptr<net::Connection> &conn_);
        ~HttpSessionContext();

        HttpRequest *get_request()
        {
            return request_ ? &*request_ : nullptr;
        }

        HttpResponse *get_response()
        {
            return response_ ? &*response_ : nullptr;
        }

        net::Connection *get_connection()
        {
            return conn_;
        }

        std::shared_ptr<net::Connection> connection() const
        {
            return conn_owner_.lock();
        }

        void set_connection(net::Connection *conn)
        {
            conn_owner_.reset();
            conn_ = conn;
        }

        void set_connection(const std::shared_ptr<net::Connection> &conn)
        {
            conn_owner_ = conn;
            conn_ = conn ? &*conn : nullptr;
        }

        void set_session(HttpSession *session)
        {
            session_ = session;
        }

        HttpSession *get_session()
        {
            return session_;
        }

        void send() const;

    public:
        bool parse();

        bool parse_from(const ::yuan::buffer::ByteBuffer &data);

        ::yuan::buffer::ByteBuffer take_leftover_buffer();

        bool write() const;

        bool is_completed();

        bool has_error() const;

        ResponseCode get_error_code() const;

        bool try_parse_request_content() const;

        void process_error(ResponseCode errorCode = ResponseCode::internal_server_error) const;

        void set_mode(Mode mode)
        {
            mode_ = mode;
        }

        inline HttpPacket *get_packet() const;

        bool is_downloading() const;

        bool ws_handoff_ = false;
        std::string ws_route_key_;
        std::string ws_client_key_;
        std::string ws_subproto_;

    private:
        void reset() const;

    private:
        Mode mode_;
        bool has_parsed_;
        std::weak_ptr<net::Connection> conn_owner_;
        Connection *conn_;
        std::unique_ptr<HttpRequest> request_;
        std::unique_ptr<HttpResponse> response_;
        HttpSession *session_;
    };
}

#endif
