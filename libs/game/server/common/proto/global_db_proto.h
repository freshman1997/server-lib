#ifndef YUAN_GAME_SERVER_COMMON_PROTO_GLOBAL_DB_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_GLOBAL_DB_PROTO_H

#include "common/codec/binary_codec.h"

#include <cstdint>
#include <string>

namespace yuan::game::server
{
    struct SSGlobalDbConfigGetRequest
    {
        std::string key;

        YUAN_GAME_BINARY_FIELDS(key)
    };

    struct SSGlobalDbConfigSetRequest
    {
        std::string key;
        std::string value;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(key, value, data_version)
    };

    struct SSGlobalDbConfigResponse
    {
        bool ok = false;
        std::string message;
        bool has_value = false;
        std::string key;
        std::string value;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(ok, message, has_value, key, value, data_version)
    };
}

#endif
