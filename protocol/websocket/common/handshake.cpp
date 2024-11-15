#include "handshake.h"
#include "base/utils/base64.h"
#include "packet.h"
#include "request.h"
#include "response_code.h"
#include "websocket_connection.h"

#include <openssl/sha.h>
#include <openssl/ssl.h>

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
        if (!key)
        {
            return false;
        }

        auto subProtocol = req->get_header("sec-websocket-protocol");
        if (subProtocol) {

        }
        
        client_key_ = *key;
        server_key_ = generate_server_key();

        ok_ = true;

        resp->set_response_code(http::ResponseCode::switch_protocol);
        resp->add_header("Connection", "Upgrade");
        resp->add_header("Upgrade", "websocket");
        resp->add_header("Sec-WebSocket-Accept", server_key_);
        resp->send();

        return true;
    }

    bool WebSocketHandshaker::do_handshake_client(http::HttpRequest * req, http::HttpResponse *resp, bool isResp)
    {
        if (!isResp) {
            req->set_method(http::HttpMethod::get_);
            req->set_version(http::HttpVersion::v_1_1);
            req->add_header("Upgrade", "websocket");
            req->add_header("Connection", "Upgrade");
            req->add_header("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
            req->add_header("Sec-WebSocket-Version", "13");
            req->send();
            return true;
        } else {
            if (!resp->is_ok()) {
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

            auto key = req->get_header("sec-websocket-accept");
            if (!key)
            {
                return false;
            }

            server_key_ = *key;
            ok_ = true;
            return true;
        }
    }

    std::string WebSocketHandshaker::generate_server_key()
    {
        std::string magic = client_key_ + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char *hash = SHA1((unsigned char *)magic.c_str(), magic.size(), nullptr);
        return base::util::base64_encode((const char *)hash);
    }
}
