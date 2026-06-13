#include "web/handler/web_handler.h"

#include "http_server.h"
#include "request.h"
#include "response.h"

#include <nlohmann/json.hpp>

namespace yuan::game::server
{
    namespace
    {
        std::string error_json(const std::string &message)
        {
            return nlohmann::json{{"ok", false}, {"message", message}}.dump();
        }

        std::string auth_json(const WebAuthResponse &auth)
        {
            auto root = nlohmann::json::parse(encode_login_options_response_json(auth.login_options));
            root["ok"] = auth.ok;
            root["player_uid"] = auth.player_uid;
            root["message"] = auth.message;
            return root.dump();
        }

        std::optional<WebAuthRequest> parse_auth_body(yuan::net::http::HttpRequest *request)
        {
            const auto *body = request->body_begin();
            const std::string text = body ? std::string(body, request->get_body_length()) : std::string();
            try {
                const auto root = nlohmann::json::parse(text);
                WebAuthRequest auth;
                auth.account = root.value("account", std::string{});
                auth.password = root.value("password", std::string{});
                return auth;
            } catch (...) {
                return std::nullopt;
            }
        }
    }

    bool register_web_http_handlers(yuan::net::http::HttpServer &server, WebHandlerContext &context)
    {
        server.on("/bootstrap", [&context](yuan::net::http::HttpRequest *request,
                                           yuan::net::http::HttpResponse *response) {
            if (!request->is_get()) {
                response->json(error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                return;
            }
            const auto player_uid_text = request->get_param("player_uid");
            if (player_uid_text.empty()) {
                response->json(error_json("missing player_uid"), yuan::net::http::ResponseCode::bad_request);
                return;
            }
            PlayerUid player_uid = 0;
            try {
                player_uid = static_cast<PlayerUid>(std::stoull(player_uid_text));
            } catch (...) {
                response->json(error_json("invalid player_uid"), yuan::net::http::ResponseCode::bad_request);
                return;
            }
            if (!context.bootstrap_provider) {
                response->json(error_json("bootstrap handler is not configured"), yuan::net::http::ResponseCode::internal_server_error);
                return;
            }
            const auto options = context.bootstrap_provider(LoginOptionsRequest{player_uid});
            if (!options) {
                response->json(error_json("world bootstrap unavailable"), yuan::net::http::ResponseCode::bad_gateway);
                return;
            }
            response->json(encode_login_options_response_json(*options));
        });

        const auto auth_handler = [&context](bool is_register,
                                             yuan::net::http::HttpRequest *request,
                                             yuan::net::http::HttpResponse *response) {
            if (!request->is_post()) {
                response->json(error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                return;
            }
            const auto auth_request = parse_auth_body(request);
            if (!auth_request) {
                response->json(error_json("invalid auth request"), yuan::net::http::ResponseCode::bad_request);
                return;
            }
            const auto &handler = is_register ? context.register_handler : context.login_handler;
            if (!handler) {
                response->json(error_json(is_register ? "register handler is not configured" : "login handler is not configured"),
                               yuan::net::http::ResponseCode::internal_server_error);
                return;
            }
            const auto auth = handler(*auth_request);
            response->json(auth_json(auth), auth.ok ? yuan::net::http::ResponseCode::ok_ : yuan::net::http::ResponseCode::bad_request);
        };

        server.on("/register", [auth_handler](yuan::net::http::HttpRequest *request,
                                               yuan::net::http::HttpResponse *response) {
            auth_handler(true, request, response);
        });

        server.on("/login", [auth_handler](yuan::net::http::HttpRequest *request,
                                            yuan::net::http::HttpResponse *response) {
            auth_handler(false, request, response);
        });

        return true;
    }
}
