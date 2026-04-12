#ifndef __MATCH_PLAYER_H__
#define __MATCH_PLAYER_H__

#include <cstdint>
#include <string>

#include "base/time.h"

namespace match
{
    enum class GameMode : uint8_t
    {
        MODE_1V1_NORMAL = 0,
        MODE_2V2_RANKED = 1,
        MODE_5V5_TEAM_1 = 2,
        MODE_5V5_TEAM_2 = 3,
        MODE_5V5_TEAM_3 = 4,
    };

    enum class PlayerState : uint8_t
    {
        IDLE = 0,
        MATCHING = 1,
        MATCHED = 2,
    };

    struct Player
    {
        uint64_t player_id;
        GameMode mode;
        uint32_t score;
        uint32_t match_range;
        PlayerState state;
        uint64_t enter_time_ms;
        std::string extra_data;

        Player()
            : player_id(0)
            , mode(GameMode::MODE_1V1_NORMAL)
            , score(0)
            , match_range(0)
            , state(PlayerState::IDLE)
            , enter_time_ms(yuan::base::time::steady_now_ms())
        {
        }

        Player(const uint64_t id, const GameMode m, const uint32_t s, const std::string& extra = "")
            : player_id(id)
            , mode(m)
            , score(s)
            , match_range(0)
            , state(PlayerState::MATCHING)
            , enter_time_ms(yuan::base::time::steady_now_ms())
            , extra_data(extra)
        {
        }

        uint32_t get_wait_time_ms() const
        {
            return static_cast<uint32_t>(yuan::base::time::steady_now_ms() - enter_time_ms);
        }
    };
}

#endif
