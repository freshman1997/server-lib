#ifndef YUAN_GAME_SERVER_COMMON_CODEC_GAME_CODEC_TYPE_H
#define YUAN_GAME_SERVER_COMMON_CODEC_GAME_CODEC_TYPE_H

#include <cstdint>

namespace yuan::game::server
{
    enum class GameCodecType : std::uint8_t
    {
        binary = 1,
        protobuf = 2,
        json = 3,
    };
}

#endif
