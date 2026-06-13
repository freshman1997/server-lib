#ifndef YUAN_GAME_SERVER_WEB_HANDLER_WEB_HANDLER_H
#define YUAN_GAME_SERVER_WEB_HANDLER_WEB_HANDLER_H

#include "common/game_messages.h"

#include <functional>

namespace yuan::game::server
{
    struct WebHandlerContext
    {
        ServiceAddress address;
        std::function<std::optional<LoginOptionsResponse>(LoginOptionsRequest)> bootstrap_provider;
        std::function<WebAuthResponse(WebAuthRequest)> register_handler;
        std::function<WebAuthResponse(WebAuthRequest)> login_handler;
    };

    bool register_web_handlers(yuan::rpc::Server &server, WebHandlerContext &context);
}

#endif
