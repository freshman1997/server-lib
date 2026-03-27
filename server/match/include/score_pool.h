#ifndef __SCORE_POOL_H__
#define __SCORE_POOL_H__

#include <cstdint>
#include <vector>
#include <memory>
#include "node.h"

namespace match
{
    // 分数池 - 存储特定分数范围内的匹配节点
    class ScorePool
    {
    public:
        ScorePool(uint32_t min_score, uint32_t max_score, int32_t pool_index);
        ~ScorePool() = default;

        // 检查分数是否在此池范围内
        bool is_in_range(uint32_t score) const;

        // 添加节点到池中
        void add_node(std::shared_ptr<MatchNode> node);

        // 从池中移除节点
        bool remove_node(uint64_t node_id);

        // 获取节点
        std::shared_ptr<MatchNode> get_node(uint64_t node_id) const;

        // 尝试匹配（返回匹配成功的节点列表）
        bool try_match(uint32_t need_count, std::vector<std::shared_ptr<MatchNode>>& matched_nodes);

        // 获取等待中的节点数量
        size_t get_waiting_count() const { return nodes_.size(); }

        // 获取分数范围
        uint32_t get_min_score() const { return min_score_; }
        uint32_t get_max_score() const { return max_score_; }
        int32_t get_pool_index() const { return pool_index_; }

        // 检查池是否为空
        bool empty() const { return nodes_.empty(); }

    private:
        uint32_t min_score_;
        uint32_t max_score_;
        int32_t pool_index_;
        std::vector<std::shared_ptr<MatchNode>> nodes_;
    };
}

#endif
