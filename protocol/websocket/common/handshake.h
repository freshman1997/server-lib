#ifndef __NET_WEBSOCKET_COMMON_HANDSHAKE_H__
#define __NET_WEBSOCKET_COMMON_HANDSHAKE_H__
#include "context.h"
#include "request.h"
#include "response.h"
#include "websocket_connection.h"

#include <string>

namespace net::websocket 
{
    class WebSocketHandshaker
    {
    public:
        
    public:
        WebSocketHandshaker();

    public:
        bool on_handshake(http::HttpRequest * req, http::HttpResponse *resp, WebSocketConnection::WorkMode workMode = WebSocketConnection::WorkMode::client_, bool isResp = false);

        bool is_handshake_done() const
        {
            return ok_;
        }

        void set_url(const std::string &url)
        {
            url_ = url;
        }

        const std::string & get_client_key() const
        {
            return client_key_;
        }

        const std::string & get_server_key() const
        {
            return server_key_;
        }

    private:
        bool do_handshake_server(http::HttpRequest * req, http::HttpResponse *resp);

        bool do_handshake_client(http::HttpRequest * req, http::HttpResponse *resp, bool isResp);
        
        std::string generate_server_key();

    private:
        bool ok_;
        std::string url_;
        std::string client_key_;
        std::string server_key_;
    };
}

#endif