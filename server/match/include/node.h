#ifndef __MATCH_NODE_H__
#define __MATCH_NODE_H__

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace match
{
    enum class NodeState : uint8_t
    {
        IDLE = 0,
        MATCHING = 1,
        MATCHED = 2,
    };

    class MatchNode
    {
    public:
        MatchNode();
        ~MatchNode() = default;

        void add_player(uint64_t player_id, uint32_t score, const std::string& extra_data = "");
        bool remove_player(uint64_t player_id);

        size_t get_player_count() const { return players_.size(); }
        uint32_t get_avg_score() const;
        uint32_t get_score_range() const;
        uint32_t get_wait_time_ms() const;

        uint32_t get_match_range() const { return match_range_; }
        void set_match_range(uint32_t range) { match_range_ = range; }

        int32_t get_current_pool_index() const { return current_pool_index_; }
        void set_current_pool_index(int32_t index) { current_pool_index_ = index; }

        NodeState get_state() const { return state_; }
        void set_state(NodeState state) { state_ = state; }

        void reset();

        const std::vector<std::pair<uint64_t, uint32_t>>& get_players() const { return players_; }

        uint64_t get_node_id() const { return node_id_; }
        void set_node_id(uint64_t id) { node_id_ = id; }

        void refresh_enter_time();

    private:
        uint64_t node_id_;
        std::vector<std::pair<uint64_t, uint32_t>> players_;
        std::string extra_data_;
        uint32_t avg_score_;
        NodeState state_;
        uint32_t match_range_;
        int32_t current_pool_index_;
        uint64_t enter_time_ms_;
    };
}

#endif
