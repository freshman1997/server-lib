#include "redis_client_pool.h"

#include "internal/redis_registry.h"
#include "value/error_value.h"

namespace yuan::redis
{
    namespace
    {
        void close_clients(std::vector<std::shared_ptr<RedisClient> > &clients)
        {
            for (auto &client : clients) {
                if (client) {
                    client->wait_in_flight(1000);
                    client->close();
                }
            }
            clients.clear();
        }
    }

    void RedisClientPool::close_impl()
    {
        closing_.store(true, std::memory_order_release);

        close_clients(clients_);
        clients_.clear();
        registries_.clear();
        next_idx_.store(0);
    }

    bool RedisClientPool::init(const Option &option, std::size_t pool_size)
    {
        std::uint64_t init_generation = 0;
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            close_impl();
            init_generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        }

        if (pool_size == 0) {
            pool_size = 1;
        }

        std::vector<std::shared_ptr<void> > registries;
        std::vector<std::shared_ptr<RedisClient> > clients;
        registries.reserve(pool_size);
        clients.reserve(pool_size);

        for (std::size_t i = 0; i < pool_size; ++i) {
            auto registry = std::make_shared<RedisRegistry>();
            auto client_option = option;
            if (!client_option.name_.empty()) {
                client_option.name_ += "-pool-" + std::to_string(i);
            }

            auto client = std::make_shared<RedisClient>(client_option, registry);
            if (!client->ping()) {
                clients.push_back(client);
                close_clients(clients);
                return false;
            }

            registries.push_back(registry);
            clients.push_back(client);
        }

        std::lock_guard<std::mutex> lock(pool_mutex_);
        if (generation_.load(std::memory_order_acquire) != init_generation) {
            close_clients(clients);
            return false;
        }

        close_impl();
        registries_ = std::move(registries);
        clients_ = std::move(clients);
        next_idx_.store(0, std::memory_order_release);
        closing_.store(false, std::memory_order_release);
        return !clients_.empty();
    }

    std::shared_ptr<RedisClient> RedisClientPool::get_round_robin_client()
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        if (clients_.empty() || closing_.load(std::memory_order_acquire)) {
            return nullptr;
        }

        const auto start = next_idx_.fetch_add(1, std::memory_order_acq_rel);
        for (std::size_t attempt = 0; attempt < clients_.size(); ++attempt) {
            const auto idx = (start + attempt) % clients_.size();
            auto client = clients_[idx];
            if (!client) {
                continue;
            }

            if (client->is_connected() || client->ensure_connected()) {
                return client;
            }
        }

        return nullptr;
    }

    void RedisClientPool::close()
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        generation_.fetch_add(1, std::memory_order_acq_rel);
        close_impl();
    }

    bool RedisClientPool::is_closing() const
    {
        return closing_.load(std::memory_order_acquire);
    }

    RedisClientPoolStats RedisClientPool::stats() const
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        RedisClientPoolStats out;
        out.closing = closing_.load(std::memory_order_acquire);
        out.size = clients_.size();
        for (const auto &client : clients_) {
            if (client && client->is_connected()) {
                ++out.connected;
            }
        }
        out.unhealthy = out.size >= out.connected ? out.size - out.connected : 0;
        return out;
    }
}
