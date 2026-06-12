#ifndef YUAN_BASE_ID_SNOWFLAKE_ID_GENERATOR_H_
#define YUAN_BASE_ID_SNOWFLAKE_ID_GENERATOR_H_

#include <chrono>
#include <cstdint>
#include <mutex>

namespace yuan::base
{
    // SnowflakeIdGenerator 生成按时间大致递增的 64 位 ID：时间戳 + 节点号 + 序列号。
    //
    // 适用场景：多节点服务生成全局趋势递增 ID，例如订单、消息、实体、任务。
    // 用法：
    //   yuan::base::SnowflakeIdGenerator ids(1);
    //   auto id = ids.next();
    class SnowflakeIdGenerator
    {
    public:
        explicit SnowflakeIdGenerator(std::uint64_t node_id)
            : node_id_(node_id & node_mask_)
        {
        }

        std::uint64_t next()
        {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch());
            return next(now);
        }

        std::uint64_t next(std::chrono::milliseconds unix_ms)
        {
            const std::uint64_t now = static_cast<std::uint64_t>(unix_ms.count());
            std::lock_guard<std::mutex> lock(mutex_);

            const std::uint64_t safe_now = now < last_ms_ ? last_ms_ : now;
            if (safe_now == last_ms_) {
                sequence_ = (sequence_ + 1) & sequence_mask_;
            } else {
                sequence_ = 0;
                last_ms_ = safe_now;
            }

            return ((safe_now - custom_epoch_ms_) << time_shift_) | (node_id_ << node_shift_) | sequence_;
        }

    private:
        static constexpr std::uint64_t custom_epoch_ms_ = 1704067200000ULL;
        static constexpr std::uint64_t sequence_bits_ = 12;
        static constexpr std::uint64_t node_bits_ = 10;
        static constexpr std::uint64_t sequence_mask_ = (1ULL << sequence_bits_) - 1;
        static constexpr std::uint64_t node_mask_ = (1ULL << node_bits_) - 1;
        static constexpr std::uint64_t node_shift_ = sequence_bits_;
        static constexpr std::uint64_t time_shift_ = sequence_bits_ + node_bits_;

        std::uint64_t node_id_ = 0;
        std::mutex mutex_;
        std::uint64_t last_ms_ = 0;
        std::uint64_t sequence_ = 0;
    };
}

#endif // YUAN_BASE_ID_SNOWFLAKE_ID_GENERATOR_H_
