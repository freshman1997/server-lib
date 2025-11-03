#include "redis_cli_manager.h"
#include <cstdlib>

namespace yuan::redis 
{
    int RedisCliManager::init(const std::vector<Option> &options)
    {
        for (const auto &opt : options)
        {
            auto redis_client = std::make_shared<RedisClient>(opt);
            m_redis_cli_map.emplace_back(opt, redis_client);
        }

        return 0;
    }

    std::shared_ptr<RedisClient> RedisCliManager::get_random_redis_client()
    {
        if (m_redis_cli_map.empty())
        {
            return nullptr;
        }

        srand(time(nullptr));

        int idx = rand() % m_redis_cli_map.size();
        return m_redis_cli_map[idx].second;
    }

    std::shared_ptr<RedisClient> RedisCliManager::get_round_robin_redis_client()
    {
        if (m_redis_cli_map.empty()) {
            return nullptr;
        }

        int idx = (m_redis_cli_idx_ + 1) % m_redis_cli_map.size();
        m_redis_cli_idx_ = idx;
        return m_redis_cli_map[idx].second;
    }

}