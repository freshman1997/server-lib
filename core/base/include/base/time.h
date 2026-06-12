#ifndef __BASE_TIME_H__
#define __BASE_TIME_H__

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>

namespace yuan::base::time
{
    namespace detail
    {
        inline std::atomic<uint32_t> time_unit_{100};
        inline std::atomic<uint64_t> tick_{0};

        inline std::atomic<bool> steady_override_enabled_{false};
        inline std::atomic<uint64_t> steady_override_ms_{0};

        inline std::atomic<bool> system_override_enabled_{false};
        inline std::atomic<uint64_t> system_override_ms_{0};
        inline std::atomic<int64_t> system_offset_seconds_{0};

        inline void advance_atomic_ms(std::atomic<uint64_t>& value, const int64_t delta_ms)
        {
            auto current = value.load(std::memory_order_relaxed);
            for (;;) {
                const auto next = delta_ms >= 0
                    ? current + static_cast<uint64_t>(delta_ms)
                    : (current > static_cast<uint64_t>(-delta_ms) ? current - static_cast<uint64_t>(-delta_ms) : 0ULL);
                if (value.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
                    return;
                }
            }
        }
    }

    inline uint64_t steady_now_ms()
    {
        if (detail::steady_override_enabled_.load(std::memory_order_relaxed)) {
            return detail::steady_override_ms_.load(std::memory_order_relaxed);
        }
        const auto time_now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(time_now.time_since_epoch()).count());
    }

    inline uint64_t steady_now_us()
    {
        if (detail::steady_override_enabled_.load(std::memory_order_relaxed)) {
            return detail::steady_override_ms_.load(std::memory_order_relaxed) * 1000ULL;
        }
        const auto time_now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(time_now.time_since_epoch()).count());
    }

    inline uint64_t system_now_ms()
    {
        if (detail::system_override_enabled_.load(std::memory_order_relaxed)) {
            return detail::system_override_ms_.load(std::memory_order_relaxed);
        }
        const auto time_now = std::chrono::system_clock::now();
        const auto now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(time_now.time_since_epoch()).count());
        const auto offset = detail::system_offset_seconds_.load(std::memory_order_relaxed) * 1000LL;
        return offset >= 0 ? now + static_cast<uint64_t>(offset)
                           : (now > static_cast<uint64_t>(-offset) ? now - static_cast<uint64_t>(-offset) : 0ULL);
    }

    inline uint64_t system_now_us()
    {
        if (detail::system_override_enabled_.load(std::memory_order_relaxed)) {
            return detail::system_override_ms_.load(std::memory_order_relaxed) * 1000ULL;
        }
        const auto time_now = std::chrono::system_clock::now();
        const auto now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(time_now.time_since_epoch()).count());
        const auto offset_us = detail::system_offset_seconds_.load(std::memory_order_relaxed) * 1000000LL;
        return offset_us >= 0 ? now + static_cast<uint64_t>(offset_us)
                              : (now > static_cast<uint64_t>(-offset_us) ? now - static_cast<uint64_t>(-offset_us) : 0ULL);
    }

    inline std::time_t system_now_seconds()
    {
        return static_cast<std::time_t>(system_now_ms() / 1000ULL);
    }

    inline uint64_t system_now_sec()
    {
        return static_cast<uint64_t>(system_now_seconds());
    }

    inline std::tm localtime(const std::time_t timestamp)
    {
        std::tm tm_buf {};
#ifdef _WIN32
        localtime_s(&tm_buf, &timestamp);
#else
        localtime_r(&timestamp, &tm_buf);
#endif
        return tm_buf;
    }

    inline std::tm gmtime(const std::time_t timestamp)
    {
        std::tm tm_buf {};
#ifdef _WIN32
        gmtime_s(&tm_buf, &timestamp);
#else
        gmtime_r(&timestamp, &tm_buf);
#endif
        return tm_buf;
    }

    inline uint64_t get_tick_count()
    {
        return steady_now_ms();
    }

    inline void init_time(const uint32_t unit)
    {
        detail::time_unit_.store(unit == 0 ? 1U : unit, std::memory_order_relaxed);
        detail::tick_.store(get_tick_count(), std::memory_order_relaxed);
    }

    inline uint64_t get_passed_time()
    {
        auto last_tick = detail::tick_.load(std::memory_order_relaxed);
        if (last_tick == 0) {
            detail::tick_.store(get_tick_count(), std::memory_order_relaxed);
            return 0;
        }

        const auto unit = static_cast<uint64_t>(detail::time_unit_.load(std::memory_order_relaxed));
        const auto cur_tick = get_tick_count();
        const auto passed = (cur_tick - last_tick) / unit;
        detail::tick_.store(last_tick + passed * unit, std::memory_order_relaxed);
        return passed;
    }

    inline uint32_t now()
    {
        return static_cast<uint32_t>(get_tick_count());
    }

    inline void set_steady_time_for_test(const uint64_t now_ms)
    {
        detail::steady_override_ms_.store(now_ms, std::memory_order_relaxed);
        detail::steady_override_enabled_.store(true, std::memory_order_relaxed);
    }

    inline void set_system_time_for_test(const uint64_t now_ms)
    {
        detail::system_override_ms_.store(now_ms, std::memory_order_relaxed);
        detail::system_override_enabled_.store(true, std::memory_order_relaxed);
    }

    inline void set_system_time_offset_seconds(const int64_t offset_seconds)
    {
        detail::system_offset_seconds_.store(offset_seconds, std::memory_order_relaxed);
    }

    inline int64_t system_time_offset_seconds()
    {
        return detail::system_offset_seconds_.load(std::memory_order_relaxed);
    }

    inline void advance_steady_time_for_test(const int64_t delta_ms)
    {
        set_steady_time_for_test(steady_now_ms());
        detail::advance_atomic_ms(detail::steady_override_ms_, delta_ms);
    }

    inline void advance_system_time_for_test(const int64_t delta_ms)
    {
        set_system_time_for_test(system_now_ms());
        detail::advance_atomic_ms(detail::system_override_ms_, delta_ms);
    }

    inline void reset_test_time()
    {
        detail::steady_override_enabled_.store(false, std::memory_order_relaxed);
        detail::system_override_enabled_.store(false, std::memory_order_relaxed);
        detail::system_offset_seconds_.store(0, std::memory_order_relaxed);
    }
}

#endif
