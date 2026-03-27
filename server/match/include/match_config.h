#ifndef __MATCH_CONFIG_H__
#define __MATCH_CONFIG_H__

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include "player.h"
#include "singleton/singleton.h"

namespace match
{
    // 分数池配置
    struct ScorePoolConfig
    {
        uint32_t min_score;     // 分数下限
        uint32_t max_score;     // 分数上限

        ScorePoolConfig() : min_score(0), max_score(0) {}
        ScorePoolConfig(uint32_t min, uint32_t max) : min_score(min), max_score(max) {}
    };

    // 区间扩展配置
    struct RangeExpandConfig
    {
        uint32_t timeout_ms;        // 超时时间（毫秒）
        uint32_t expand_pools;      // 扩展池子数量
    };

    // 玩法配置
    struct ModeConfig
    {
        GameMode mode;                          // 玩法类型
        std::string mode_name;                  // 玩法名称
        uint32_t player_count;                  // 需要的玩家数量
        uint32_t max_team_size;                 // 最大队伍人数（不能超过player_count的一半）
        std::vector<ScorePoolConfig> pools;     // 分数池配置
        std::vector<RangeExpandConfig> expands; // 区间扩展配置
        uint32_t max_expand_pools;              // 最大扩展池数量
        bool enabled;                           // 是否启用

        ModeConfig()
            : mode(GameMode::MODE_1V1_NORMAL)
            , player_count(2)
            , max_team_size(1)
            , max_expand_pools(3)
            , enabled(true)
        {}
    };

    // 匹配服务器配置
    class MatchConfig : public yuan::singleton::Singleton<MatchConfig>
    {
    public:
        MatchConfig();
        ~MatchConfig() = default;

        // 从JSON文件加载配置
        bool load_from_file(const std::string& file_path);
        
        // 保存配置到JSON文件
        bool save_to_file(const std::string& file_path);

        // 获取玩法配置
        const ModeConfig* get_mode_config(GameMode mode) const;

        // 获取所有启用的玩法
        std::vector<GameMode> get_enabled_modes() const;

        // 检查配置是否有效
        bool is_valid() const { return valid_; }

        uint32_t get_match_interval() const { return match_interval_ms_; }
        uint32_t get_monitor_interval() const { return monitor_interval_ms_; }

    private:
        bool parse_mode_config(const std::string& json_content);
        void init_default_config();

        bool valid_;
        uint32_t match_interval_ms_;
        uint32_t monitor_interval_ms_;
        std::unordered_map<GameMode, ModeConfig> mode_configs_;
    };
}

#endif
