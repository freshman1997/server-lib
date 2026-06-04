#include "redis_client_pool.h"

#include "internal/redis_registry.h"
#include "value/error_value.h"

#include <chrono>

namespace yuan::redis
{
    namespace
    {
        void close_clients(std::vector<std::shared_ptr<RedisClient> > &clients, uint32_t wait_ms = 5000)
        {
            for (auto &client : clients) {
                if (client) {
                    client->wait_in_flight(wait_ms);
                    client->close();
                }
            }
            clients.clear();
        }
    }

    RedisClientPool::~RedisClientPool()
    {
        close();
    }

    bool RedisClientPool::init(const Option &option, std::size_t pool_size)
    {
        closing_.store(true, std::memory_order_release);
        stop_health_check();

        std::uint64_t init_generation = 0;
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            generation_.fetch_add(1, std::memory_order_acq_rel);
            init_generation = generation_.load(std::memory_order_acquire);
            close_clients(clients_, 1000);
            clients_.clear();
            registries_.clear();
            next_idx_.store(0);
        }

        if (pool_size == 0) {
            pool_size = 1;
        }

        option_ = option;

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
                close_clients(clients, 1000);
                return false;
            }

            registries.push_back(registry);
            clients.push_back(client);
        }

        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
if (generation_.load(std::memory_order_acquire) != init_generation) {
                close_clients(clients, 1000);
                return false;
            }

            registries_ = std::move(registries);
            clients_ = std::move(clients);
            next_idx_.store(0, std::memory_order_release);
            closing_.store(false, std::memory_order_release);
        }

        health_checks_.store(0, std::memory_order_release);
        health_check_successes_.store(0, std::memory_order_release);
        health_check_failures_.store(0, std::memory_order_release);
        health_check_skips_.store(0, std::memory_order_release);

        start_health_check();
        notify_client_available();
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

    std::shared_ptr<RedisClient> RedisClientPool::get_client_with_wait(uint32_t timeout_ms)
    {
        if (timeout_ms == 0) {
            return get_round_robin_client();
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (true) {
            auto client = get_round_robin_client();
            if (client) {
                return client;
            }

            if (closing_.load(std::memory_order_acquire)) {
                return nullptr;
            }

            auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining_ms.count() <= 0) {
                wait_timeouts_.fetch_add(1, std::memory_order_release);
                return nullptr;
            }

            wait_attempts_.fetch_add(1, std::memory_order_release);

            {
                std::unique_lock<std::mutex> lock(client_available_mutex_);
                auto wait_time = std::min(remaining_ms, std::chrono::milliseconds(100));
                client_available_cv_.wait_for(lock, wait_time);
            }

            if (closing_.load(std::memory_order_acquire)) {
                return nullptr;
            }
        }
    }

    void RedisClientPool::notify_client_available()
    {
        client_available_cv_.notify_all();
    }

void RedisClientPool::close()
    {
        const uint32_t default_wait = option_.pool_wait_timeout_ms_ > 0 ? option_.pool_wait_timeout_ms_ : 5000;
        close(default_wait);
    }

    void RedisClientPool::close(uint32_t wait_in_flight_ms)
    {
        closing_.store(true, std::memory_order_release);
        stop_health_check();

        std::lock_guard<std::mutex> lock(pool_mutex_);
        generation_.fetch_add(1, std::memory_order_acq_rel);
        close_clients(clients_, wait_in_flight_ms);
        clients_.clear();
        registries_.clear();
        next_idx_.store(0);
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
        out.health_checks = health_checks_.load(std::memory_order_acquire);
        out.health_check_successes = health_check_successes_.load(std::memory_order_acquire);
        out.health_check_failures = health_check_failures_.load(std::memory_order_acquire);
        out.health_check_skips = health_check_skips_.load(std::memory_order_acquire);
        out.wait_attempts = wait_attempts_.load(std::memory_order_acquire);
        out.wait_timeouts = wait_timeouts_.load(std::memory_order_acquire);
        return out;
    }

    void RedisClientPool::start_health_check()
    {
        if (option_.health_check_interval_ms_ <= 0) {
            return;
        }

        health_check_thread_ = std::thread([this]() { health_check_loop(); });
    }

    void RedisClientPool::stop_health_check()
    {
        health_check_cv_.notify_all();
        if (health_check_thread_.joinable()) {
            health_check_thread_.join();
        }
    }

    void RedisClientPool::health_check_loop()
    {
        while (!closing_.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lock(health_check_mutex_);
                if (health_check_cv_.wait_for(lock,
                    std::chrono::milliseconds(option_.health_check_interval_ms_),
                    [this]() { return closing_.load(std::memory_order_acquire); })) {
                    break;
                }
            }

            if (closing_.load(std::memory_order_acquire)) {
                break;
            }

            std::vector<std::shared_ptr<RedisClient> > snapshot;
            {
                std::lock_guard<std::mutex> lock(pool_mutex_);
                if (closing_.load(std::memory_order_acquire)) {
                    break;
                }
                snapshot = clients_;
            }

            for (auto &client : snapshot) {
                if (!client) {
                    continue;
                }

                if (closing_.load(std::memory_order_acquire)) {
                    break;
                }

                if (!client->is_connected()) {
                    if (client->ensure_connected()) {
                        health_checks_.fetch_add(1, std::memory_order_release);
                        health_check_successes_.fetch_add(1, std::memory_order_release);
                    } else {
                        health_checks_.fetch_add(1, std::memory_order_release);
                        health_check_failures_.fetch_add(1, std::memory_order_release);
                    }
                    continue;
                }

                auto result = client->try_ping();
                switch (result) {
                case HealthCheckResult::ok:
                    health_checks_.fetch_add(1, std::memory_order_release);
                    health_check_successes_.fetch_add(1, std::memory_order_release);
                    break;
                case HealthCheckResult::busy:
                    health_checks_.fetch_add(1, std::memory_order_release);
                    health_check_skips_.fetch_add(1, std::memory_order_release);
                    break;
                case HealthCheckResult::disconnected:
                    health_checks_.fetch_add(1, std::memory_order_release);
                    health_check_failures_.fetch_add(1, std::memory_order_release);
                    break;
                case HealthCheckResult::failed:
                    health_checks_.fetch_add(1, std::memory_order_release);
                    health_check_failures_.fetch_add(1, std::memory_order_release);
                    break;
                }
            }

            notify_client_available();
        }
    }
}