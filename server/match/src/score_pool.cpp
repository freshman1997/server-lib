#include "score_pool.h"
#include <algorithm>

namespace match
{
    ScorePool::ScorePool(uint32_t min_score, uint32_t max_score, int32_t pool_index)
        : min_score_(min_score)
        , max_score_(max_score)
        , pool_index_(pool_index)
    {
    }

    bool ScorePool::is_in_range(uint32_t score) const
    {
        return score >= min_score_ && score < max_score_;
    }

    void ScorePool::add_node(std::shared_ptr<MatchNode> node)
    {
        if (node)
        {
            node->set_current_pool_index(pool_index_);
            nodes_.push_back(node);
        }
    }

    bool ScorePool::remove_node(uint64_t node_id)
    {
        auto it = std::find_if(nodes_.begin(), nodes_.end(),
            [node_id](const std::shared_ptr<MatchNode>& n) {
                return n && n->get_node_id() == node_id;
            });

        if (it != nodes_.end())
        {
            (*it)->set_current_pool_index(-1);
            nodes_.erase(it);
            return true;
        }
        return false;
    }

    std::shared_ptr<MatchNode> ScorePool::get_node(uint64_t node_id) const
    {
        for (const auto& node : nodes_)
        {
            if (node && node->get_node_id() == node_id)
            {
                return node;
            }
        }
        return nullptr;
    }

    bool ScorePool::try_match(uint32_t need_count, std::vector<std::shared_ptr<MatchNode>>& matched_nodes)
    {
        if (nodes_.empty())
        {
            return false;
        }

        // 计算当前池中已有的玩家总数
        uint32_t total_players = 0;
        for (const auto& node : nodes_)
        {
            if (node && node->get_state() == NodeState::MATCHING)
            {
                total_players += static_cast<uint32_t>(node->get_player_count());
            }
        }

        if (total_players < need_count)
        {
            return false;
        }

        // 尝试找到足够的玩家
        matched_nodes.clear();
        uint32_t collected = 0;

        for (auto& node : nodes_)
        {
            if (node && node->get_state() == NodeState::MATCHING)
            {
                matched_nodes.push_back(node);
                collected += static_cast<uint32_t>(node->get_player_count());

                if (collected >= need_count)
                {
                    break;
                }
            }
        }

        // 检查是否收集够了（不能超过需要人数太多）
        if (collected < need_count)
        {
            matched_nodes.clear();
            return false;
        }

        // 检查最后一个节点是否导致超出太多，如果超出超过一个节点的人数则不匹配
        // 但允许稍微超出（比如需要10人，收集了11人，这种情况可以接受）
        if (collected > need_count)
        {
            // 检查是否可以去掉最后一个节点仍然满足需求
            size_t last_count = matched_nodes.back()->get_player_count();
            if (collected - last_count >= need_count)
            {
                // 可以去掉最后一个节点
                collected -= static_cast<uint32_t>(last_count);
                matched_nodes.pop_back();
            }
        }

        return matched_nodes.size() > 0 && collected >= need_count;
    }
}
