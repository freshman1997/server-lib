#ifndef __MATCH_PLAYER_H__
#define __MATCH_PLAYER_H__

#include <cstdint>
#include <string>
#include <chrono>

namespace match
{
    // 玩法类型
    enum class GameMode : uint8_t
    {
        MODE_1V1_NORMAL = 0,    // 双人常规玩法
        MODE_2V2_RANKED = 1,    // 2v2排位玩法
        MODE_5V5_TEAM_1 = 2,    // 5v5团队竞技1
        MODE_5V5_TEAM_2 = 3,    // 5v5团队竞技2
        MODE_5V5_TEAM_3 = 4,    // 5v5团队竞技3
    };

    // 玩家状态
    enum class PlayerState : uint8_t
    {
        IDLE = 0,           // 空闲
        MATCHING = 1,       // 匹配中
        MATCHED = 2,        // 已匹配
    };

    // 玩家信息
    struct Player
    {
        uint64_t player_id;                     // 玩家ID
        GameMode mode;                          // 游戏模式
        uint32_t score;                         // 分数（分段/创号时间戳，根据模式不同含义不同）
        uint32_t match_range;                   // 当前匹配区间
        PlayerState state;                      // 玩家状态
        std::chrono::steady_clock::time_point enter_time;  // 进入匹配池时间
        std::string extra_data;                 // 额外数据（JSON格式）

        Player() 
            : player_id(0)
            , mode(GameMode::MODE_1V1_NORMAL)
            , score(0)
            , match_range(0)
            , state(PlayerState::IDLE)
            , enter_time(std::chrono::steady_clock::now())
        {}

        Player(uint64_t id, GameMode m, uint32_t s, const std::string& extra = "")
            : player_id(id)
            , mode(m)
            , score(s)
            , match_range(0)
            , state(PlayerState::MATCHING)
            , enter_time(std::chrono::steady_clock::now())
            , extra_data(extra)
        {}

        // 获取已等待时间（毫秒）
        uint32_t get_wait_time_ms() const
        {
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - enter_time);
            return static_cast<uint32_t>(duration.count());
        }
    };
}

#endif
