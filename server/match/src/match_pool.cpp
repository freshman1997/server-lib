#include "match_pool.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <random>

namespace match
{
    MatchPool::MatchPool()
        : room_id_counter_(0)
        , initialized_(false)
    {
    }

    MatchPool::~MatchPool()
    {
    }

    bool MatchPool::init(const std::string& config_path)
    {
        if (!MatchConfig::get_instance()->is_valid())
        {
            return false;
        }

        // 初始化各玩法的匹配池
        auto modes = MatchConfig::get_instance()->get_enabled_modes();
        if (modes.empty())
        {
            return false;
        }

        for (auto mode : modes)
        {
            const ModeConfig* cfg = MatchConfig::get_instance()->get_mode_config(mode);
            if (!cfg || !cfg->enabled)
            {
                continue;
            }

            ModePool pool;
            pool.mode = mode;

            // 创建分数池
            int32_t pool_index = 0;
            for (const auto& pool_cfg : cfg->pools)
            {
                pool.pools.push_back(std::make_unique<ScorePool>(
                    pool_cfg.min_score, pool_cfg.max_score, pool_index));
                pool_index++;
            }

            mode_pools_[mode] = std::move(pool);
        }

        // 确保默认玩法池存在（用于存放新创建的节点）
        if (mode_pools_.find(GameMode::MODE_1V1_NORMAL) == mode_pools_.end())
        {
            ModePool pool;
            pool.mode = GameMode::MODE_1V1_NORMAL;
            mode_pools_[GameMode::MODE_1V1_NORMAL] = std::move(pool);
        }

        initialized_ = true;
        return true;
    }

    std::shared_ptr<MatchNode> MatchPool::create_node(uint64_t player_id, uint32_t score,
                                                       const std::string& extra_data)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 检查玩家是否已经在其他节点中
        if (player_node_map_.find(player_id) != player_node_map_.end())
        {
            return nullptr;
        }

        auto node = std::make_shared<MatchNode>();
        node->add_player(player_id, score, extra_data);
        node->set_node_id(player_id);

        player_node_map_[player_id] = player_id;

        // 将节点添加到默认玩法池中（IDLE状态，等待开始匹配）
        // 使用 MODE_1V1_NORMAL 作为默认存放位置
        auto& default_pool = mode_pools_[GameMode::MODE_1V1_NORMAL];
        default_pool.nodes[player_id] = node;

        return node;
    }

    bool MatchPool::add_player_to_node(uint64_t node_id, uint64_t player_id, 
                                        uint32_t score, const std::string& extra_data)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 检查玩家是否已经在其他节点中
        if (player_node_map_.find(player_id) != player_node_map_.end())
        {
            return false;
        }

        // 查找节点
        for (auto& mode_pair : mode_pools_)
        {
            auto& pool = mode_pair.second;
            auto it = pool.nodes.find(node_id);
            if (it != pool.nodes.end())
            {
                const ModeConfig* cfg = MatchConfig::get_instance()->get_mode_config(pool.mode);
                if (cfg && it->second->get_player_count() >= cfg->max_team_size)
                {
                    return false;  // 队伍已满
                }

                it->second->add_player(player_id, score, extra_data);
                player_node_map_[player_id] = node_id;
                return true;
            }
        }

        return false;
    }

    bool MatchPool::remove_player_from_node(uint64_t node_id, uint64_t player_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto node = get_node(node_id);
        if (!node)
        {
            return false;
        }

        if (node->remove_player(player_id))
        {
            player_node_map_.erase(player_id);
            return true;
        }

        return false;
    }

    std::shared_ptr<MatchNode> MatchPool::get_node(uint64_t node_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (const auto& mode_pair : mode_pools_)
        {
            auto it = mode_pair.second.nodes.find(node_id);
            if (it != mode_pair.second.nodes.end())
            {
                return it->second;
            }
        }

        return nullptr;
    }

    std::shared_ptr<MatchNode> MatchPool::find_node_by_player(uint64_t player_id) const
    {
        auto it = player_node_map_.find(player_id);
        if (it == player_node_map_.end())
        {
            return nullptr;
        }

        return get_node(it->second);
    }

    int32_t MatchPool::find_pool_index(ModePool& mode_pool, uint32_t score) const
    {
        for (size_t i = 0; i < mode_pool.pools.size(); ++i)
        {
            if (mode_pool.pools[i]->is_in_range(score))
            {
                return static_cast<int32_t>(i);
            }
        }
        return -1;
    }

    std::vector<int32_t> MatchPool::get_candidate_pools(int32_t center_index, uint32_t expand_range,
                                                         size_t total_pools) const
    {
        std::vector<int32_t> indices;

        // 从中心向两边扩展
        for (uint32_t offset = 0; offset <= expand_range && offset < total_pools; ++offset)
        {
            if (offset == 0)
            {
                indices.push_back(center_index);
            }
            else
            {
                // 向上扩展
                int32_t up = center_index + static_cast<int32_t>(offset);
                if (up >= 0 && up < static_cast<int32_t>(total_pools))
                {
                    indices.push_back(up);
                }

                // 向下扩展
                int32_t down = center_index - static_cast<int32_t>(offset);
                if (down >= 0 && down < static_cast<int32_t>(total_pools) && down != center_index)
                {
                    indices.push_back(down);
                }
            }
        }

        return indices;
    }

    void MatchPool::update_node_range(MatchNode& node, const ModeConfig* config)
    {
        uint32_t wait_time = node.get_wait_time_ms();
        uint32_t expand_pools = 0;

        for (const auto& expand : config->expands)
        {
            if (wait_time >= expand.timeout_ms)
            {
                expand_pools = expand.expand_pools;
            }
        }

        expand_pools = std::min(expand_pools, config->max_expand_pools);
        node.set_match_range(expand_pools);
    }

    bool MatchPool::start_match(uint64_t node_id, GameMode mode)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        const ModeConfig* cfg = MatchConfig::get_instance()->get_mode_config(mode);
        if (!cfg || !cfg->enabled)
        {
            return false;
        }

        // 在所有玩法池中查找节点（可能还在默认池中）
        std::shared_ptr<MatchNode> node;
        ModePool* source_pool = nullptr;
        
        for (auto& mode_pair : mode_pools_)
        {
            auto it = mode_pair.second.nodes.find(node_id);
            if (it != mode_pair.second.nodes.end())
            {
                node = it->second;
                source_pool = &mode_pair.second;
                break;
            }
        }

        if (!node)
        {
            return false;  // 节点不存在
        }

        if (node->get_state() != NodeState::IDLE)
        {
            return false;  // 已经在匹配中
        }

        // 检查队伍人数
        if (node->get_player_count() > cfg->max_team_size)
        {
            return false;
        }

        // 获取目标玩法池
        auto& target_pool = mode_pools_[mode];

        // 找到对应的分数池
        int32_t pool_index = find_pool_index(target_pool, node->get_avg_score());
        if (pool_index < 0)
        {
            return false;
        }

        // 如果需要移动到不同的玩法池
        if (source_pool != &target_pool)
        {
            // 从源池中移除
            source_pool->nodes.erase(node_id);
            // 添加到目标池
            target_pool.nodes[node_id] = node;
        }

        // 设置节点状态并加入分数池
        node->set_state(NodeState::MATCHING);
        node->set_match_range(0);
        node->refresh_enter_time();
        node->set_current_pool_index(pool_index);
        target_pool.pools[pool_index]->add_node(node);

        return true;
    }

    bool MatchPool::cancel_match(uint64_t node_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto& mode_pair : mode_pools_)
        {
            auto& pool = mode_pair.second;
            auto it = pool.nodes.find(node_id);
            if (it != pool.nodes.end())
            {
                auto& node = it->second;
                if (node->get_state() == NodeState::MATCHING)
                {
                    // 从当前池中移除
                    int32_t pool_idx = node->get_current_pool_index();
                    if (pool_idx >= 0 && pool_idx < static_cast<int32_t>(pool.pools.size()))
                    {
                        pool.pools[pool_idx]->remove_node(node_id);
                    }

                    node->reset();
                    return true;
                }
            }
        }

        return false;
    }

    bool MatchPool::player_offline(uint64_t player_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto node = find_node_by_player(player_id);
        if (!node)
        {
            return false;
        }

        // 取消匹配
        cancel_match(node->get_node_id());

        // 移除所有玩家
        for (const auto& player : node->get_players())
        {
            player_node_map_.erase(player.first);
        }

        // 从所有模式池中移除节点
        for (auto& mode_pair : mode_pools_)
        {
            mode_pair.second.nodes.erase(node->get_node_id());
        }

        return true;
    }

    void MatchPool::do_match()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_)
        {
            return;
        }

        for (auto& mode_pair : mode_pools_)
        {
            GameMode mode = mode_pair.first;
            ModePool& pool = mode_pair.second;
            const ModeConfig* config = MatchConfig::get_instance()->get_mode_config(mode);

            if (!config || !config->enabled)
            {
                continue;
            }

            match_mode(mode, pool, config);
        }
    }

    void MatchPool::match_mode(GameMode mode, ModePool& pool, const ModeConfig* config)
    {
        // 先更新所有匹配中节点的匹配范围
        for (auto& pair : pool.nodes)
        {
            if (pair.second->get_state() == NodeState::MATCHING)
            {
                update_node_range(*pair.second, config);
            }
        }

        // 尝试在每个池子中匹配
        for (auto& score_pool : pool.pools)
        {
            if (score_pool->empty())
            {
                continue;
            }

            std::vector<std::shared_ptr<MatchNode>> matched_nodes;
            if (try_match_in_pool(pool, score_pool.get(), config, matched_nodes))
            {
                // 标记节点为已匹配
                for (auto& node : matched_nodes)
                {
                    node->set_state(NodeState::MATCHED);
                }

                // 从池中移除
                remove_matched_nodes(pool, matched_nodes);
                pool.match_count++;

                // 触发回调
                if (match_callback_)
                {
                    MatchResult result;
                    result.success = true;
                    result.mode = mode;
                    result.matched_nodes = matched_nodes;
                    result.room_id = generate_room_id();
                    match_callback_(result);
                }
            }
        }
    }

    bool MatchPool::try_match_in_pool(ModePool& pool, ScorePool* score_pool,
                                       const ModeConfig* config,
                                       std::vector<std::shared_ptr<MatchNode>>& matched_nodes)
    {
        if (!score_pool || score_pool->empty())
        {
            return false;
        }

        // 统计当前池中的玩家数量
        uint32_t total_players = 0;
        std::vector<std::shared_ptr<MatchNode>> candidates;

        for (const auto& pair : pool.nodes)
        {
            if (pair.second->get_state() == NodeState::MATCHING &&
                pair.second->get_current_pool_index() == score_pool->get_pool_index())
            {
                candidates.push_back(pair.second);
                total_players += static_cast<uint32_t>(pair.second->get_player_count());
            }
        }

        if (total_players < config->player_count)
        {
            return false;
        }

        // 贪心选择节点，尽量让接近的玩家匹配
        matched_nodes.clear();
        uint32_t collected = 0;

        // 按等待时间排序，优先匹配等待时间长的
        std::sort(candidates.begin(), candidates.end(),
            [](const std::shared_ptr<MatchNode>& a, const std::shared_ptr<MatchNode>& b) {
                return a->get_wait_time_ms() > b->get_wait_time_ms();
            });

        for (auto& node : candidates)
        {
            // 检查加入后是否会超出太多
            uint32_t new_total = collected + static_cast<uint32_t>(node->get_player_count());
            if (new_total > config->player_count + config->max_team_size)
            {
                continue;  // 跳过会导致超出太多的节点
            }

            matched_nodes.push_back(node);
            collected = new_total;

            if (collected >= config->player_count)
            {
                break;
            }
        }

        if (collected < config->player_count)
        {
            matched_nodes.clear();
            return false;
        }

        return true;
    }

    void MatchPool::remove_matched_nodes(ModePool& pool,
                                          const std::vector<std::shared_ptr<MatchNode>>& nodes)
    {
        for (const auto& node : nodes)
        {
            // 从分数池中移除
            int32_t pool_idx = node->get_current_pool_index();
            if (pool_idx >= 0 && pool_idx < static_cast<int32_t>(pool.pools.size()))
            {
                pool.pools[pool_idx]->remove_node(node->get_node_id());
            }

            // 移除玩家映射
            for (const auto& player : node->get_players())
            {
                player_node_map_.erase(player.first);
            }

            // 从节点映射中移除
            pool.nodes.erase(node->get_node_id());
        }
    }

    void MatchPool::set_match_callback(MatchCallback callback)
    {
        match_callback_ = callback;
    }

    std::string MatchPool::get_statistics() const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        std::stringstream ss;
        ss << "{\n";

        for (const auto& mode_pair : mode_pools_)
        {
            const ModeConfig* cfg = MatchConfig::get_instance()->get_mode_config(mode_pair.first);
            ss << "  \"" << (cfg ? cfg->mode_name : "unknown") << "\": {\n";
            ss << "    \"waiting\": " << mode_pair.second.total_waiting() << ",\n";
            ss << "    \"nodes\": " << mode_pair.second.nodes.size() << ",\n";
            ss << "    \"pools\": " << mode_pair.second.pools.size() << ",\n";
            ss << "    \"matched\": " << mode_pair.second.match_count << "\n";
            ss << "  },\n";
        }

        ss << "  \"total_players\": " << player_node_map_.size() << "\n";
        ss << "}\n";

        return ss.str();
    }

    std::unordered_map<GameMode, uint32_t> MatchPool::get_waiting_counts() const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        std::unordered_map<GameMode, uint32_t> counts;
        for (const auto& pair : mode_pools_)
        {
            counts[pair.first] = static_cast<uint32_t>(pair.second.total_waiting());
        }
        return counts;
    }

    std::string MatchPool::generate_room_id()
    {
        std::stringstream ss;
        ss << "room_" << std::setw(10) << std::setfill('0') << (++room_id_counter_);

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(1000, 9999);
        ss << "_" << dis(gen);

        return ss.str();
    }
}
