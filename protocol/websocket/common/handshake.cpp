#include "handshake.h"
#include "base/utils/base64.h"
#include "base/utils/string_util.h"
#include "packet.h"
#include "request.h"
#include "response_code.h"
#include "websocket_config.h"
#include "websocket_connection.h"
#include "websocket_utils.h"

#include <cstring>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <sstream>

namespace yuan::net::websocket 
{
    WebSocketHandshaker::WebSocketHandshaker() : ok_(false)
    {
        client_key_ = WebSocketConfigManager::get_instance()->get_client_key();
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

        client_key_ = *key;
        server_key_ = WebSocketUtils::generate_server_key(client_key_);


        auto subProtocol = req->get_header("sec-websocket-protocol");
        if (subProtocol) {
            decode_into_set(*subProtocol, client_sub_protos_, ',');

            // 服务器必须在响应中明确表示接受哪个子协议，否则连接将不会成功建立。
            const auto *supportProto = WebSocketConfigManager::get_instance()->find_server_support_sub_protocol(client_sub_protos_);
            if (supportProto) {
                working_subproto_ = *supportProto;
                resp->add_header("Sec-WebSocket-Protocol", working_subproto_);
            } else {
                return false;
            }
        }

        auto extensions = req->get_header("sec-websocket-extensions");
        if (extensions) {
            // permessage-deflate; client_max_window_bits
            // decode_into_set()
        }

        resp->set_response_code(http::ResponseCode::switch_protocol);
        resp->add_header("Connection", "Upgrade");
        resp->add_header("Upgrade", "websocket");
        resp->add_header("Sec-WebSocket-Accept", server_key_);
        resp->send();

        ok_ = true;

        return true;
    }

    bool WebSocketHandshaker::do_handshake_client(http::HttpRequest * req, http::HttpResponse *resp, bool isResp)
    {
        if (!isResp) {
            req->set_method(http::HttpMethod::get_);
            req->set_version(http::HttpVersion::v_1_1);
            req->add_header("Upgrade", "websocket");
            req->add_header("Connection", "Upgrade");
            req->add_header("Sec-WebSocket-Key", client_key_);
            req->add_header("Sec-WebSocket-Version", "13");

            const auto & supportProtos = WebSocketConfigManager::get_instance()->get_client_support_subprotos();
            if (!supportProtos.empty()) {
                req->add_header("Sec-WebSocket-Protocol", encode_sub_proto(supportProtos));
            }

            req->send();
            return true;
        } else {
            if (!resp->is_ok()) {
                return false;
            }

            auto conn = resp->get_header("connection");
            if (!conn || (*conn != "Upgrade" && *conn != "upgrade"))
            {
                return false;
            }

            auto upgrade = resp->get_header("upgrade");
            if (!upgrade || (*upgrade != "websocket" && *upgrade != "Websocket"))
            {
                return false;
            }

            auto key = resp->get_header("sec-websocket-accept");
            if (!key)
            {
                return false;
            }

            if (client_key_.empty() || WebSocketUtils::generate_server_key(client_key_) != *key) {
                return false;
            }

            if (!WebSocketConfigManager::get_instance()->get_client_support_subprotos().empty()) {
                auto subProtocol = resp->get_header("sec-websocket-protocol");

                // 服务器必须在响应中明确表示接受哪个子协议，否则连接将不会成功建立。
                if (!subProtocol) {
                    return false;
                }

                decode_into_set(*subProtocol, server_sub_protos_, ',');

                const auto *supportProto = WebSocketConfigManager::get_instance()->find_server_support_sub_protocol(server_sub_protos_);
                if (!supportProto) {
                    return false;
                }
                working_subproto_ = *supportProto;
            }
            

            server_key_ = *key;
            ok_ = true;
            return true;
        }
    }

    void WebSocketHandshaker::decode_into_set(const std::string &raw, std::set<std::string> &protos, char delimiter)
    {
        // json, mqtt
        if (raw.empty()) {
            return;
        }

        std::string word;
        for (int i = 0; i < raw.size(); ) {
            if (raw[i] == ' ') {
                ++i;
                continue;
            }

            while (i <= raw.size()) {
                if (i + 1 <= raw.size() && raw[i] != delimiter) {
                    word.push_back(raw[i]);
                    ++i;
                } else {
                    protos.insert(word);

                    ++i;
                    word.clear();

                    break;
                }
            }
        }
    }

    std::string WebSocketHandshaker::encode_sub_proto(const std::set<std::string> &protos)
    {
        std::stringstream ss;
        std::size_t counter = 0;
        for (const auto &proto : protos) {
            ss << proto;
            if (counter < protos.size()) {
                ss << ", ";
            }
            ++counter;
        }
        return ss.str();
    }
}
