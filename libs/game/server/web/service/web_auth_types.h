#ifndef YUAN_GAME_SERVER_WEB_SERVICE_WEB_AUTH_TYPES_H
#define YUAN_GAME_SERVER_WEB_SERVICE_WEB_AUTH_TYPES_H

#include "common/proto/login_proto.h"

#include <string>

namespace yuan::game::server
{
    struct WebAuthRequest
    {
        std::string account;
        std::string password;
    };

    struct WebAuthResponse
    {
        bool ok = false;
        PlayerUid player_uid = 0;
        LoginOptionsResponse login_options;
        std::string message;
    };

    struct WebCreateRoleRequest
    {
        PlayerUid player_uid = 0;
        std::string name;
    };

    struct WebCreateRoleResponse
    {
        bool ok = false;
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::string message;
    };
}

#endif
