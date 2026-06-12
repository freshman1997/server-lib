#ifndef YUAN_GAME_SERVER_WEB_WEB_SERVICE_H
#define YUAN_GAME_SERVER_WEB_WEB_SERVICE_H

#include "common/game_messages.h"

#include <functional>

namespace yuan::game::server
{
    class WebService : public ServiceNode
    {
    public:
        using BootstrapProvider = std::function<std::optional<LoginOptionsResponse>(LoginOptionsRequest)>;
        using AuthHandler = std::function<WebAuthResponse(WebAuthRequest)>;

        explicit WebService(ServiceAddress address);

        void set_bootstrap_provider(BootstrapProvider provider);
        void set_register_handler(AuthHandler handler);
        void set_login_handler(AuthHandler handler);

    private:
        BootstrapProvider bootstrap_provider_;
        AuthHandler register_handler_;
        AuthHandler login_handler_;
    };
}

#endif
