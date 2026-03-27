#ifndef __MATCH_POOL_H__
#define __MATCH_POOL_H__

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include "node.h"
#include "score_pool.h"
#include "match_config.h"

namespace match
{
    // 匹配结果
    struct MatchResult
    {
        bool success;
        GameMode mode;
        std::vector<std::shared_ptr<MatchNode>> matched_nodes;  // 匹配成功的节点
        std::string room_id;
        std::string error_msg;

        MatchResult() : success(false), mode(GameMode::MODE_1V1_NORMAL) {}
    };

    // 匹配池管理器
    class MatchPool
    {
    public:
        using MatchCallback = std::function<void(const MatchResult&)>;

        MatchPool();
        ~MatchPool();

        // 初始化
        bool init(const std::string& config_path);

        // 创建匹配节点（单人或组队）
        std::shared_ptr<MatchNode> create_node(uint64_t player_id, uint32_t score, 
                                               const std::string& extra_data = "");

        // 添加玩家到已有节点（组队）
        bool add_player_to_node(uint64_t node_id, uint64_t player_id, uint32_t score,
                               const std::string& extra_data = "");

        // 从节点移除玩家
        bool remove_player_from_node(uint64_t node_id, uint64_t player_id);

        // 获取节点
        std::shared_ptr<MatchNode> get_node(uint64_t node_id) const;

        // 玩家开始匹配（将节点加入匹配池）
        bool start_match(uint64_t node_id, GameMode mode);

        // 玩家取消匹配
        bool cancel_match(uint64_t node_id);

        // 玩家下线（移除节点）
        bool player_offline(uint64_t player_id);

        // 执行匹配检查（定时调用）
        void do_match();

        // 设置匹配成功回调
        void set_match_callback(MatchCallback callback);

        // 获取统计信息
        std::string get_statistics() const;

        // 获取各玩法等待人数
        std::unordered_map<GameMode, uint32_t> get_waiting_counts() const;

    private:
        // 单个玩法的匹配池
        struct ModePool
        {
            GameMode mode;
            std::vector<std::unique_ptr<ScorePool>> pools;      // 分数池列表
            std::unordered_map<uint64_t, std::shared_ptr<MatchNode>> nodes;  // 所有节点
            uint32_t match_count;

            ModePool() : match_count(0) {}

            size_t total_waiting() const {
                size_t total = 0;
                for (const auto& pool : pools) {
                    total += pool->get_waiting_count();
                }
                return total;
            }
        };

        // 根据分数找到对应的池子索引
        int32_t find_pool_index(ModePool& mode_pool, uint32_t score) const;

        // 根据当前池索引和扩展范围找到候选池子
        std::vector<int32_t> get_candidate_pools(int32_t center_index, uint32_t expand_range, 
                                                  size_t total_pools) const;

        // 更新节点的匹配范围
        void update_node_range(MatchNode& node, const ModeConfig* config);

        // 执行单个玩法的匹配
        void match_mode(GameMode mode, ModePool& pool, const ModeConfig* config);

        // 尝试在指定池子中匹配
        bool try_match_in_pool(ModePool& pool, ScorePool* score_pool, 
                              const ModeConfig* config,
                              std::vector<std::shared_ptr<MatchNode>>& matched_nodes);

        // 从池中移除已匹配的节点
        void remove_matched_nodes(ModePool& pool, 
                                 const std::vector<std::shared_ptr<MatchNode>>& nodes);

        // 生成房间ID
        std::string generate_room_id();

        // 查找玩家所在的节点
        std::shared_ptr<MatchNode> find_node_by_player(uint64_t player_id) const;

    private:
        std::unordered_map<GameMode, ModePool> mode_pools_;
        std::unordered_map<uint64_t, uint64_t> player_node_map_;  // player_id -> node_id
        mutable std::mutex mutex_;
        MatchCallback match_callback_;
        uint64_t room_id_counter_;
        bool initialized_;
    };
}

#endif
