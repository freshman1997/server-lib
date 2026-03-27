#ifndef __MATCH_NODE_H__
#define __MATCH_NODE_H__

#include <cstdint>
#include <vector>
#include <chrono>
#include <string>

namespace match
{
    // 节点状态
    enum class NodeState : uint8_t
    {
        IDLE = 0,           // 空闲，等待开始匹配
        MATCHING = 1,       // 匹配中
        MATCHED = 2,        // 已匹配
    };

    // 匹配节点 - 代表一个玩家或一个队伍
    class MatchNode
    {
    public:
        MatchNode();
        ~MatchNode() = default;

        // 添加玩家到节点（组队）
        void add_player(uint64_t player_id, uint32_t score, const std::string& extra_data = "");

        // 移除玩家
        bool remove_player(uint64_t player_id);

        // 获取节点总人数
        size_t get_player_count() const { return players_.size(); }

        // 获取队伍平均分数
        uint32_t get_avg_score() const;

        // 获取队伍分数范围（最高分-最低分）
        uint32_t get_score_range() const;

        // 获取已等待时间（毫秒）
        uint32_t get_wait_time_ms() const;

        // 获取当前匹配范围
        uint32_t get_match_range() const { return match_range_; }

        // 设置当前匹配范围
        void set_match_range(uint32_t range) { match_range_ = range; }

        // 获取当前池子索引
        int32_t get_current_pool_index() const { return current_pool_index_; }

        // 设置当前池子索引
        void set_current_pool_index(int32_t index) { current_pool_index_ = index; }

        // 获取状态
        NodeState get_state() const { return state_; }

        // 设置状态
        void set_state(NodeState state) { state_ = state; }

        // 重置匹配状态
        void reset();

        // 获取玩家列表
        const std::vector<std::pair<uint64_t, uint32_t>>& get_players() const { return players_; }

        // 获取节点ID（使用第一个玩家ID）
        uint64_t get_node_id() const { return node_id_; }

        // 设置节点ID
        void set_node_id(uint64_t id) { node_id_ = id; }

        // 刷新进入时间
        void refresh_enter_time();

    private:
        uint64_t node_id_;                                          // 节点ID
        std::vector<std::pair<uint64_t, uint32_t>> players_;        // 玩家列表 <player_id, score>
        std::string extra_data_;                                    // 额外数据
        uint32_t avg_score_;                                        // 平均分数（缓存）
        NodeState state_;                                           // 状态
        uint32_t match_range_;                                      // 当前匹配范围
        int32_t current_pool_index_;                                // 当前所在的池子索引，-1表示不在任何池子
        std::chrono::steady_clock::time_point enter_time_;          // 进入匹配池时间
    };
}

#endif
