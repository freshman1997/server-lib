#include "match_config.h"
#include "player.h"

namespace match
{
    MatchConfig::MatchConfig()
        : valid_(false)
        , match_interval_ms_(1000)
        , monitor_interval_ms_(5000)
    {
        init_default_config();
    }

    void MatchConfig::init_default_config()
    {
        // 1v1 常规玩法 - 按创号时间匹配
        ModeConfig mode1v1;
        mode1v1.mode = GameMode::MODE_1V1_NORMAL;
        mode1v1.mode_name = "1v1_normal";
        mode1v1.player_count = 2;
        mode1v1.max_team_size = 1;  // 单人匹配
        mode1v1.pools = {
            ScorePoolConfig(0, 86400),           // 0-1天
            ScorePoolConfig(86400, 259200),      // 1-3天
            ScorePoolConfig(259200, 604800),     // 3-7天
            ScorePoolConfig(604800, 2592000),    // 7-30天
            ScorePoolConfig(2592000, 7776000),   // 30-90天
            ScorePoolConfig(7776000, UINT32_MAX) // 90天以上
        };
        mode1v1.expands = {
            {10000, 1},  // 10秒后扩展1个池
            {20000, 2},  // 20秒后扩展2个池
            {30000, 3}   // 30秒后扩展3个池
        };
        mode1v1.max_expand_pools = 3;
        mode1v1.enabled = true;
        mode_configs_[GameMode::MODE_1V1_NORMAL] = mode1v1;

        // 2v2 排位玩法 - 按分段匹配
        ModeConfig mode2v2;
        mode2v2.mode = GameMode::MODE_2V2_RANKED;
        mode2v2.mode_name = "2v2_ranked";
        mode2v2.player_count = 4;
        mode2v2.max_team_size = 2;  // 最多2人组队
        mode2v2.pools = {
            ScorePoolConfig(0, 400),       // 青铜
            ScorePoolConfig(400, 800),     // 白银
            ScorePoolConfig(800, 1200),    // 黄金
            ScorePoolConfig(1200, 1600),   // 铂金
            ScorePoolConfig(1600, 2000),   // 钻石
            ScorePoolConfig(2000, UINT32_MAX) // 大师以上
        };
        mode2v2.expands = {
            {15000, 1},
            {30000, 2},
            {45000, 3}
        };
        mode2v2.max_expand_pools = 3;
        mode2v2.enabled = true;
        mode_configs_[GameMode::MODE_2V2_RANKED] = mode2v2;

        // 5v5 团队竞技1
        ModeConfig mode5v5_1;
        mode5v5_1.mode = GameMode::MODE_5V5_TEAM_1;
        mode5v5_1.mode_name = "5v5_team_1";
        mode5v5_1.player_count = 10;
        mode5v5_1.max_team_size = 5;  // 最多5人组队
        mode5v5_1.pools = {
            ScorePoolConfig(0, 5),      // 段位1-5
            ScorePoolConfig(5, 10),     // 段位6-10
            ScorePoolConfig(10, 15),    // 段位11-15
            ScorePoolConfig(15, UINT32_MAX)
        };
        mode5v5_1.expands = {
            {20000, 1},
            {40000, 2}
        };
        mode5v5_1.max_expand_pools = 2;
        mode5v5_1.enabled = true;
        mode_configs_[GameMode::MODE_5V5_TEAM_1] = mode5v5_1;

        // 5v5 团队竞技2
        ModeConfig mode5v5_2;
        mode5v5_2.mode = GameMode::MODE_5V5_TEAM_2;
        mode5v5_2.mode_name = "5v5_team_2";
        mode5v5_2.player_count = 10;
        mode5v5_2.max_team_size = 5;
        mode5v5_2.pools = {
            ScorePoolConfig(0, 5),
            ScorePoolConfig(5, 10),
            ScorePoolConfig(10, 15),
            ScorePoolConfig(15, UINT32_MAX)
        };
        mode5v5_2.expands = {
            {20000, 1},
            {40000, 2}
        };
        mode5v5_2.max_expand_pools = 2;
        mode5v5_2.enabled = true;
        mode_configs_[GameMode::MODE_5V5_TEAM_2] = mode5v5_2;

        // 5v5 团队竞技3
        ModeConfig mode5v5_3;
        mode5v5_3.mode = GameMode::MODE_5V5_TEAM_3;
        mode5v5_3.mode_name = "5v5_team_3";
        mode5v5_3.player_count = 10;
        mode5v5_3.max_team_size = 5;
        mode5v5_3.pools = {
            ScorePoolConfig(0, 5),
            ScorePoolConfig(5, 10),
            ScorePoolConfig(10, 15),
            ScorePoolConfig(15, UINT32_MAX)
        };
        mode5v5_3.expands = {
            {20000, 1},
            {40000, 2}
        };
        mode5v5_3.max_expand_pools = 2;
        mode5v5_3.enabled = true;
        mode_configs_[GameMode::MODE_5V5_TEAM_3] = mode5v5_3;

        valid_ = true;
    }

    bool MatchConfig::load_from_file(const std::string& file_path)
    {
        // TODO: 实现JSON配置加载
        // 目前使用默认配置
        return valid_;
    }

    bool MatchConfig::save_to_file(const std::string& file_path)
    {
        // TODO: 实现配置保存
        return false;
    }

    const ModeConfig* MatchConfig::get_mode_config(GameMode mode) const
    {
        auto it = mode_configs_.find(mode);
        if (it != mode_configs_.end())
        {
            return &it->second;
        }
        return nullptr;
    }

    std::vector<GameMode> MatchConfig::get_enabled_modes() const
    {
        std::vector<GameMode> modes;
        for (const auto& pair : mode_configs_)
        {
            if (pair.second.enabled)
            {
                modes.push_back(pair.first);
            }
        }
        return modes;
    }
}
