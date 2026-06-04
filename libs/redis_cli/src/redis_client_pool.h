#ifndef __YUAN_REDIS_CLIENT_POOL_H__
#define __YUAN_REDIS_CLIENT_POOL_H__

#include "option.h"
#include "redis_client.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace yuan::redis
{
    struct RedisClientPoolStats
    {
        bool closing = false;
        std::size_t size = 0;
        std::size_t connected = 0;
        std::size_t unhealthy = 0;
        std::uint64_t health_checks = 0;
        std::uint64_t health_check_successes = 0;
        std::uint64_t health_check_failures = 0;
        std::uint64_t health_check_skips = 0;
        std::uint64_t wait_attempts = 0;
        std::uint64_t wait_timeouts = 0;
    };

    class RedisClientPool
    {
    public:
        RedisClientPool() = default;
        RedisClientPool(const RedisClientPool &) = delete;
        RedisClientPool &operator=(const RedisClientPool &) = delete;
        ~RedisClientPool();

        bool init(const Option &option, std::size_t pool_size);
        std::shared_ptr<RedisClient> get_round_robin_client();
        std::shared_ptr<RedisClient> get_client_with_wait(uint32_t timeout_ms);
        void close();
        void close(uint32_t wait_in_flight_ms);
        bool is_closing() const;
        RedisClientPoolStats stats() const;

    private:
        void start_health_check();
        void stop_health_check();
        void health_check_loop();
        void notify_client_available();

        Option option_;
        std::atomic<bool> closing_{false};
        std::atomic<std::uint64_t> generation_{0};
        std::atomic<std::size_t> next_idx_{0};
        mutable std::mutex pool_mutex_;
        std::vector<std::shared_ptr<void> > registries_;
        std::vector<std::shared_ptr<RedisClient> > clients_;
        std::thread health_check_thread_;
        std::mutex health_check_mutex_;
        std::condition_variable health_check_cv_;
        std::condition_variable client_available_cv_;
        std::mutex client_available_mutex_;
        std::atomic<std::uint64_t> health_checks_{0};
        std::atomic<std::uint64_t> health_check_successes_{0};
        std::atomic<std::uint64_t> health_check_failures_{0};
        std::atomic<std::uint64_t> health_check_skips_{0};
        std::atomic<std::uint64_t> wait_attempts_{0};
        std::atomic<std::uint64_t> wait_timeouts_{0};
    };
}

#endif // __YUAN_REDIS_CLIENT_POOL_H__
