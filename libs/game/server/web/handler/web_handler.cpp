#include "web/handler/web_handler.h"

namespace yuan::game::server
{
    bool register_web_handlers(yuan::rpc::Server &server, WebHandlerContext &context)
    {
        const bool bootstrap_registered = server.register_handler(game_route::web_bootstrap(), [&context](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());

            const auto request = decode_login_options_request(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid web bootstrap request";
                return response;
            }
            if (!context.bootstrap_provider) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "web bootstrap provider is not configured";
                return response;
            }
            const auto options = context.bootstrap_provider(*request);
            if (!options) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "world bootstrap unavailable";
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_login_options_response(*options, response.payload);
            return response;
        });

        const bool register_registered = server.register_handler(game_route::web_register(), [&context](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto request = decode_web_auth_request(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid web register request";
                return response;
            }
            const auto auth = context.register_handler ? context.register_handler(*request) : WebAuthResponse{false, 0, {}, "register handler is not configured"};
            response.status = auth.ok ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::bad_request;
            (void)encode_web_auth_response(auth, response.payload);
            return response;
        });

        const bool login_registered = server.register_handler(game_route::web_login(), [&context](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto request = decode_web_auth_request(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid web login request";
                return response;
            }
            const auto auth = context.login_handler ? context.login_handler(*request) : WebAuthResponse{false, 0, {}, "login handler is not configured"};
            response.status = auth.ok ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::bad_request;
            (void)encode_web_auth_response(auth, response.payload);
            return response;
        });

        return bootstrap_registered && register_registered && login_registered;
    }
}
