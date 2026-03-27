#include "node.h"
#include <algorithm>

namespace match
{
    MatchNode::MatchNode()
        : node_id_(0)
        , avg_score_(0)
        , state_(NodeState::IDLE)
        , match_range_(0)
        , current_pool_index_(-1)
        , enter_time_(std::chrono::steady_clock::now())
    {
    }

    void MatchNode::add_player(uint64_t player_id, uint32_t score, const std::string& extra_data)
    {
        players_.emplace_back(player_id, score);
        extra_data_ = extra_data;

        // 重新计算平均分
        if (!players_.empty())
        {
            uint64_t total = 0;
            for (const auto& p : players_)
            {
                total += p.second;
            }
            avg_score_ = static_cast<uint32_t>(total / players_.size());
        }

        // 设置节点ID为第一个玩家ID
        if (node_id_ == 0)
        {
            node_id_ = player_id;
        }
    }

    bool MatchNode::remove_player(uint64_t player_id)
    {
        auto it = std::find_if(players_.begin(), players_.end(),
            [player_id](const auto& p) { return p.first == player_id; });

        if (it != players_.end())
        {
            players_.erase(it);

            // 重新计算平均分
            if (!players_.empty())
            {
                uint64_t total = 0;
                for (const auto& p : players_)
                {
                    total += p.second;
                }
                avg_score_ = static_cast<uint32_t>(total / players_.size());
                node_id_ = players_[0].first;  // 更新节点ID
            }
            else
            {
                avg_score_ = 0;
                node_id_ = 0;
            }
            return true;
        }
        return false;
    }

    uint32_t MatchNode::get_avg_score() const
    {
        return avg_score_;
    }

    uint32_t MatchNode::get_score_range() const
    {
        if (players_.empty()) return 0;

        auto minmax = std::minmax_element(players_.begin(), players_.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

        return minmax.second->second - minmax.first->second;
    }

    uint32_t MatchNode::get_wait_time_ms() const
    {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - enter_time_);
        return static_cast<uint32_t>(duration.count());
    }

    void MatchNode::reset()
    {
        state_ = NodeState::IDLE;
        match_range_ = 0;
        current_pool_index_ = -1;
        enter_time_ = std::chrono::steady_clock::now();
    }

    void MatchNode::refresh_enter_time()
    {
        enter_time_ = std::chrono::steady_clock::now();
    }
}
