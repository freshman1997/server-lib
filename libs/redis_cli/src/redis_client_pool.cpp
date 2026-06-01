#include "redis_client_pool.h"

#include "internal/redis_registry.h"
#include "value/error_value.h"

namespace yuan::redis
{
    void RedisClientPool::close_impl()
    {
        closing_.store(true, std::memory_order_release);

        for (auto &client : clients_) {
            if (client) {
                client->wait_in_flight(1000);
                client->close();
            }
        }
        clients_.clear();
        registries_.clear();
        next_idx_.store(0);
    }

    bool RedisClientPool::init(const Option &option, std::size_t pool_size)
    {
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            close_impl();
        }
        if (pool_size == 0) {
            pool_size = 1;
        }

        closing_.store(false, std::memory_order_release);
        registries_.reserve(pool_size);
        clients_.reserve(pool_size);
        for (std::size_t i = 0; i < pool_size; ++i) {
            auto registry = std::make_shared<RedisRegistry>();
            auto client_option = option;
            if (!client_option.name_.empty()) {
                client_option.name_ += "-pool-" + std::to_string(i);
            }

            auto client = std::make_shared<RedisClient>(client_option, registry);
            if (!client->ping()) {
                std::lock_guard<std::mutex> lock2(pool_mutex_);
                close_impl();
                return false;
            }

            registries_.push_back(registry);
            clients_.push_back(client);
        }

        next_idx_.store(0);
        return !clients_.empty();
    }

    std::shared_ptr<RedisClient> RedisClientPool::get_round_robin_client()
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        if (clients_.empty() || closing_.load(std::memory_order_acquire)) {
            return nullptr;
        }

        const auto idx = next_idx_.fetch_add(1) % clients_.size();
        auto client = clients_[idx];

        if (client && !client->is_connected()) {
            client->ensure_connected();
        }

        return client;
    }

    void RedisClientPool::close()
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        closing_.store(true, std::memory_order_release);

        for (auto &client : clients_) {
            if (client) {
                client->wait_in_flight(1000);
                client->close();
            }
        }
        clients_.clear();
        registries_.clear();
        next_idx_.store(0);
    }

    bool RedisClientPool::is_closing() const
    {
        return closing_.load(std::memory_order_acquire);
    }
}
