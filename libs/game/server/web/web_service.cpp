#include "web/web_service.h"

#include <utility>

namespace yuan::game::server
{
    WebService::WebService(ServiceAddress address)
        : ServiceNode(std::move(address))
    {
        (void)rpc_server().register_handler(game_route::web_bootstrap(), [this](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());

            const auto request = decode_login_options_request(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid web bootstrap request";
                return response;
            }
            if (!bootstrap_provider_) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "web bootstrap provider is not configured";
                return response;
            }
            const auto options = bootstrap_provider_(*request);
            if (!options) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "world bootstrap unavailable";
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_login_options_response(*options, response.payload);
            return response;
        });

        (void)rpc_server().register_handler(game_route::web_register(), [this](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto request = decode_web_auth_request(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid web register request";
                return response;
            }
            const auto auth = register_handler_ ? register_handler_(*request) : WebAuthResponse{false, 0, {}, "register handler is not configured"};
            response.status = auth.ok ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::bad_request;
            (void)encode_web_auth_response(auth, response.payload);
            return response;
        });

        (void)rpc_server().register_handler(game_route::web_login(), [this](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto request = decode_web_auth_request(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid web login request";
                return response;
            }
            const auto auth = login_handler_ ? login_handler_(*request) : WebAuthResponse{false, 0, {}, "login handler is not configured"};
            response.status = auth.ok ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::bad_request;
            (void)encode_web_auth_response(auth, response.payload);
            return response;
        });
    }

    void WebService::set_bootstrap_provider(BootstrapProvider provider)
    {
        bootstrap_provider_ = std::move(provider);
    }

    void WebService::set_register_handler(AuthHandler handler)
    {
        register_handler_ = std::move(handler);
    }

    void WebService::set_login_handler(AuthHandler handler)
    {
        login_handler_ = std::move(handler);
    }
}
