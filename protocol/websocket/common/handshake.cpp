#include "handshake.h"
#include "base/utils/base64.h"
#include "base/utils/string_util.h"
#include "packet.h"
#include "header_key.h"
#include "request.h"
#include "response_code.h"
#include "websocket_config.h"
#include "websocket_connection.h"
#include "websocket_utils.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include "openssl/sha.h"
#include "openssl/ssl.h"
#include <sstream>

namespace yuan::net::websocket
{
    static bool iequals(const std::string & a, const std::string & b)
    {
        if (a.size() != b.size())
            return false;
#ifdef _WIN32
        return _stricmp(a.c_str(), b.c_str()) == 0;
#else
        return strcasecmp(a.c_str(), b.c_str()) == 0;
#endif
    }

    static std::string trim_ascii(std::string_view value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.remove_suffix(1);
        }
        return std::string(value);
    }

    static bool header_has_token(const std::string &raw, const std::string &token)
    {
        std::size_t start = 0;
        while (start <= raw.size()) {
            const auto end = raw.find(',', start);
            const auto len = (end == std::string::npos) ? std::string::npos : end - start;
            if (iequals(trim_ascii(std::string_view(raw).substr(start, len)), token)) {
                return true;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return false;
    }

    static bool is_valid_websocket_key(const std::string &raw)
    {
        const std::string key = trim_ascii(raw);
        if (key.empty() || key.size() % 4 != 0) {
            return false;
        }

        bool seen_padding = false;
        for (char ch : key) {
            const auto uch = static_cast<unsigned char>(ch);
            const bool is_base64_char = std::isalnum(uch) || ch == '+' || ch == '/';
            if (ch == '=') {
                seen_padding = true;
                continue;
            }
            if (seen_padding || !is_base64_char) {
                return false;
            }
        }

        return base::util::base64_decode(key).size() == 16;
    }

    WebSocketHandshaker::WebSocketHandshaker()
        : ok_(false), config_(nullptr)
    {
    }

    bool WebSocketHandshaker::on_handshake(http::HttpRequest * req, http::HttpResponse * resp, WorkMode workMode, bool isResp)
    {
        ok_ = false;

        if (workMode == WorkMode::server_) {
            return do_handshake_server(req, resp);
        } else {
            return do_handshake_client(req, resp, isResp);
        }
    }

    bool WebSocketHandshaker::do_handshake_server(http::HttpRequest * req, http::HttpResponse * resp)
    {
        if (req->get_method() != http::HttpMethod::get_ || req->get_version() != http::HttpVersion::v_1_1) {
            return false;
        }

        auto conn = req->get_header("connection");
        if (!conn || !header_has_token(*conn, "Upgrade")) {
            return false;
        }

        auto upgrade = req->get_header("upgrade");
        if (!upgrade || !iequals(*upgrade, "websocket")) {
            return false;
        }

        auto version = req->get_header("sec-websocket-version");
        if (!version || *version != "13") {
            return false;
        }

        auto key = req->get_header("sec-websocket-key");
        if (!key || !is_valid_websocket_key(*key)) {
            return false;
        }

        auto origin = req->get_header(http::http_header_key::origin);
        if (origin && config_ && !config_->is_origin_allowed(*origin)) {
            return false;
        }

        if (config_ && !config_->is_request_authorized(*req)) {
            return false;
        }

        client_key_ = trim_ascii(*key);
        server_key_ = WebSocketUtils::generate_server_key(client_key_);

        auto subProtocol = req->get_header("sec-websocket-protocol");
        if (subProtocol) {
            decode_into_set(*subProtocol, client_sub_protos_, ',');

            // 服务器必须在响应中明确表示接受哪个子协议，否则连接将不会成功建立。
            const auto *supportProto = config_ ? config_->find_server_support_sub_protocol(client_sub_protos_) : nullptr;
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

    bool WebSocketHandshaker::do_handshake_client(http::HttpRequest * req, http::HttpResponse * resp, bool isResp)
    {
        if (!isResp) {
            if (client_key_.empty() && config_) {
                client_key_ = config_->get_client_key();
            }

            req->set_method(http::HttpMethod::get_);
            req->set_version(http::HttpVersion::v_1_1);
            req->add_header("Upgrade", "websocket");
            req->add_header("Connection", "Upgrade");
            req->add_header("Sec-WebSocket-Key", client_key_);
            req->add_header("Sec-WebSocket-Version", "13");

            if (config_) {
                const auto &supportProtos = config_->get_client_support_subprotos();
                if (!supportProtos.empty()) {
                    req->add_header("Sec-WebSocket-Protocol", encode_sub_proto(supportProtos));
                }
            }

            req->send();
            return true;
        } else {
            if (!resp->is_ok()) {
                return false;
            }

            auto conn = resp->get_header("connection");
            if (!conn || !header_has_token(*conn, "Upgrade")) {
                return false;
            }

            auto upgrade = resp->get_header("upgrade");
            if (!upgrade || !iequals(*upgrade, "websocket")) {
                return false;
            }

            auto key = resp->get_header("sec-websocket-accept");
            if (!key) {
                return false;
            }

            if (client_key_.empty() || WebSocketUtils::generate_server_key(client_key_) != *key) {
                return false;
            }

            if (config_ && !config_->get_client_support_subprotos().empty()) {
                auto subProtocol = resp->get_header("sec-websocket-protocol");

                if (!subProtocol) {
                    return false;
                }

                decode_into_set(*subProtocol, server_sub_protos_, ',');

                const auto *supportProto = config_->find_server_support_sub_protocol(server_sub_protos_);
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

    void WebSocketHandshaker::decode_into_set(const std::string & raw, std::set<std::string> & protos, char delimiter)
    {
        // json, mqtt
        protos.clear();
        if (raw.empty()) {
            return;
        }

        std::size_t start = 0;
        while (start <= raw.size()) {
            const auto end = raw.find(delimiter, start);
            const auto len = (end == std::string::npos) ? std::string::npos : end - start;
            auto word = trim_ascii(std::string_view(raw).substr(start, len));
            if (!word.empty()) {
                protos.insert(std::move(word));
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }

    std::string WebSocketHandshaker::encode_sub_proto(const std::set<std::string> & protos)
    {
        std::stringstream ss;
        std::size_t counter = 0;
        for (const auto &proto : protos) {
            if (counter > 0) {
                ss << ", ";
            }
            ss << proto;
            ++counter;
        }
        return ss.str();
    }
}
