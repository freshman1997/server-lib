#ifndef __YUAN_REDIS_CLIENT_POOL_H__
#define __YUAN_REDIS_CLIENT_POOL_H__

#include "option.h"
#include "redis_client.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace yuan::redis
{
    struct RedisClientPoolStats
    {
        bool closing = false;
        std::size_t size = 0;
        std::size_t connected = 0;
        std::size_t unhealthy = 0;
    };

    class RedisClientPool
    {
    public:
        RedisClientPool() = default;
        RedisClientPool(const RedisClientPool &) = delete;
        RedisClientPool &operator=(const RedisClientPool &) = delete;

        bool init(const Option &option, std::size_t pool_size);
        std::shared_ptr<RedisClient> get_round_robin_client();
        void close();
        bool is_closing() const;
        RedisClientPoolStats stats() const;

    private:
        void close_impl();
        std::atomic<bool> closing_{false};
        std::atomic<std::uint64_t> generation_{0};
        std::atomic<std::size_t> next_idx_{0};
        mutable std::mutex pool_mutex_;
        std::vector<std::shared_ptr<void> > registries_;
        std::vector<std::shared_ptr<RedisClient> > clients_;
    };
}

#endif // __YUAN_REDIS_CLIENT_POOL_H__
