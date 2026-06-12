#ifndef YUAN_GAME_BASE_ID_GENERATOR_H
#define YUAN_GAME_BASE_ID_GENERATOR_H

#include "game_base/types.h"

#include <atomic>
#include <mutex>

namespace yuan::game_base
{
    class IdGenerator
    {
    public:
        explicit IdGenerator(std::uint64_t start = 1)
            : next_(start == 0 ? 1 : start)
        {
        }

        std::uint64_t next()
        {
            std::uint64_t value = next_.fetch_add(1, std::memory_order_relaxed);
            while (value == 0) {
                value = next_.fetch_add(1, std::memory_order_relaxed);
            }
            return value;
        }

    private:
        std::atomic<std::uint64_t> next_;
    };

    class SnowflakeIdGenerator
    {
    public:
        explicit SnowflakeIdGenerator(NodeId node_id)
            : node_id_(node_id & node_mask_)
        {
        }

        std::uint64_t next(std::chrono::milliseconds unix_ms)
        {
            const std::uint64_t now = static_cast<std::uint64_t>(unix_ms.count());
            std::uint64_t sequence = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (now == last_ms_) {
                    sequence_ = (sequence_ + 1) & sequence_mask_;
                } else {
                    sequence_ = 0;
                    last_ms_ = now;
                }
                sequence = sequence_;
            }
            return ((now - custom_epoch_ms_) << time_shift_) | (node_id_ << node_shift_) | sequence;
        }

    private:
        static constexpr std::uint64_t custom_epoch_ms_ = 1704067200000ULL;
        static constexpr std::uint64_t sequence_bits_ = 12;
        static constexpr std::uint64_t node_bits_ = 10;
        static constexpr std::uint64_t sequence_mask_ = (1ULL << sequence_bits_) - 1;
        static constexpr std::uint64_t node_mask_ = (1ULL << node_bits_) - 1;
        static constexpr std::uint64_t node_shift_ = sequence_bits_;
        static constexpr std::uint64_t time_shift_ = sequence_bits_ + node_bits_;

        NodeId node_id_ = 0;
        std::mutex mutex_;
        std::uint64_t last_ms_ = 0;
        std::uint64_t sequence_ = 0;
    };
}

#endif
