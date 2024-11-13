#include "handshake.h"
#include "base/utils/base64.h"
#include "request.h"
#include "response_code.h"
#include "websocket_connection.h"

namespace net::websocket 
{
    WebSocketHandshaker::WebSocketHandshaker() : ok_(false)
    {

    }

    bool WebSocketHandshaker::on_handshake(http::HttpRequest * req, http::HttpResponse *resp, WebSocketConnection::WorkMode workMode, bool isResp)
    {
        ok_ = false;

        if (workMode == WebSocketConnection::WorkMode::server_) {
            return do_handshake_server(req, resp);
        } else {
            return do_handshake_client(req, resp, isResp);
        }
    }

    bool WebSocketHandshaker::do_handshake_server(http::HttpRequest * req, http::HttpResponse *resp)
    {
        if (req->get_method() != http::HttpMethod::get_ || req->get_version() != http::HttpVersion::v_1_1)
        {
            return false;
        }

        auto conn = req->get_header("connection");
        if (!conn || (*conn != "Upgrade" && *conn != "upgrade"))
        {
            return false;
        }

        auto upgrade = req->get_header("upgrade");
        if (!upgrade || (*upgrade != "websocket" && *upgrade != "Websocket"))
        {
            return false;
        }

        auto version = req->get_header("sec-websocket-version");
        if (!version || *version != "13")
        {
            return false;
        }

        auto key = req->get_header("sec-websocket-key");
        if (!version)
        {
            return false;
        }

        resp->set_response_code(http::ResponseCode::switch_protocol);
        resp->add_header("Connection", "Upgrade");
        resp->add_header("Upgrade", "websocket");
        resp->add_header("Sec-WebSocket-Accept", "websocket");
        
        client_key_ = *key;
        server_key_ = generate_server_key();

        ok_ = true;

        resp->send();

        return true;
    }

    bool WebSocketHandshaker::do_handshake_client(http::HttpRequest * req, http::HttpResponse *resp, bool isResp)
    {
        return false;
    }

    std::string WebSocketHandshaker::generate_server_key()
    {
        std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string hash = "sha1" + client_key_;
        return base::util::base64_encode(magic + hash);
    }
}
