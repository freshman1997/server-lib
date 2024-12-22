#ifndef __NET_WEBSOCKET_COMMON_HANDSHAKE_H__
#define __NET_WEBSOCKET_COMMON_HANDSHAKE_H__
#include "context.h"
#include "request.h"
#include "response.h"
#include "websocket_connection.h"

#include <set>
#include <string>

namespace yuan::net::websocket 
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

        const std::string & get_client_key() const
        {
            return client_key_;
        }

        const std::string & get_server_key() const
        {
            return server_key_;
        }

        const std::string & get_working_subproto() const
        {
            return working_subproto_;
        }

    private:
        bool do_handshake_server(http::HttpRequest * req, http::HttpResponse *resp);

        bool do_handshake_client(http::HttpRequest * req, http::HttpResponse *resp, bool isResp);

        void decode_into_set(const std::string &raw, std::set<std::string> &protos, char delimiter);

        std::string encode_sub_proto(const std::set<std::string> &protos);

    private:
        bool ok_;
        std::string client_key_;
        std::string server_key_;
        std::string working_subproto_;
        std::set<std::string> client_sub_protos_;
        std::set<std::string> server_sub_protos_;
    };
}

#endif