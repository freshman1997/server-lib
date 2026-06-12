#ifndef YUAN_GAME_SERVER_COMMON_CACHE_CACHE_TYPES_H
#define YUAN_GAME_SERVER_COMMON_CACHE_CACHE_TYPES_H

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace yuan::game::server::cache
{
    enum class CacheEntryState : std::uint8_t
    {
        ready = 0,
        dirty = 1,
        flushing = 2,
        removed = 3
    };

    enum class CachePressure : std::uint8_t
    {
        normal = 0,
        elevated = 1,
        high = 2,
        critical = 3
    };

    enum class CacheStoreStatus : std::uint8_t
    {
        ok = 0,
        not_found = 1,
        error = 2
    };

    struct CacheLimits
    {
        std::size_t max_entries = 100000;
        std::size_t max_estimated_bytes = 256ull * 1024ull * 1024ull;
        std::size_t max_dirty_entries = 20000;
        std::size_t default_entry_bytes = 256;
        double low_watermark = 0.75;
        double high_watermark = 0.90;
        std::chrono::milliseconds idle_ttl{300000};
        bool flush_dirty_on_evict = true;
    };

    struct CacheStats
    {
        std::size_t entries = 0;
        std::size_t dirty_entries = 0;
        std::size_t pinned_entries = 0;
        std::size_t estimated_bytes = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t loads = 0;
        std::uint64_t saves = 0;
        std::uint64_t evictions = 0;
        std::uint64_t rejected = 0;
    };
}

#endif
