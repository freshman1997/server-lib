#include "redis_cli_manager.h"
#include "base/time.h"
#include <random>

namespace yuan::redis
{
    void RedisCliManager::release_all()
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        m_redis_cli_map.clear();
    }

    int RedisCliManager::init(const std::vector<Option> & options)
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        for (const auto &opt : options) {
            auto redis_client = std::make_shared<RedisClient>(opt);
            auto res = redis_client->ping();
            if (redis_client->get_last_error() || !res) {
                continue;
            }

            m_redis_cli_map.emplace_back(opt, redis_client);
        }

        return 0;
    }

    std::shared_ptr<RedisClient> RedisCliManager::get_random_redis_client()
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        if (m_redis_cli_map.empty()) {
            return nullptr;
        }

        static thread_local std::mt19937 gen(std::hash<std::thread::id> {}(std::this_thread::get_id()));
        std::uniform_int_distribution<std::size_t> dist(0, m_redis_cli_map.size() - 1);
        return m_redis_cli_map[dist(gen)].second;
    }

    std::shared_ptr<RedisClient> RedisCliManager::get_round_robin_redis_client()
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        if (m_redis_cli_map.empty()) {
            return nullptr;
        }

        int idx = m_redis_cli_idx_.fetch_add(1) % static_cast<int>(m_redis_cli_map.size());
        return m_redis_cli_map[idx].second;
    }
}
