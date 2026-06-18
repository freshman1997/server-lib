#ifndef YUAN_GAME_SERVER_COMMON_CODEC_GAME_BINARY_CODEC_H
#define YUAN_GAME_SERVER_COMMON_CODEC_GAME_BINARY_CODEC_H

#include "common/proto/proto_all.h"
#include "common/codec/binary_codec.h"

#include <optional>
#include <string>

namespace yuan::game::server
{
    template <typename T>
    bool encode_binary(const T &message, yuan::rpc::Bytes &out)
    {
        return binary_codec::write_versioned(out, [&](binary_codec::Writer &writer) {
            message.binary_encode(writer);
        });
    }

    template <typename T>
    std::optional<T> decode_binary(const yuan::rpc::Bytes &in)
    {
        return binary_codec::read_versioned<T>(in, [](binary_codec::Reader &reader, T &message) {
            return message.binary_decode(reader);
        });
    }

    std::string encode_login_options_response_json(const LoginOptionsResponse &response);
    std::optional<LoginOptionsResponse> decode_login_options_response_json(const std::string &json_text);

    std::optional<SSPlayerZoneUpdate> decode_player_zone_update(const yuan::rpc::Bytes &in);
}

#endif
