#include "common/cache/cache.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    struct PlayerProfile
    {
        std::uint64_t id = 0;
        std::string name;
        std::vector<int> items;
    };

    struct PlayerProfileSize
    {
        std::size_t operator()(const PlayerProfile &profile) const
        {
            return sizeof(PlayerProfile) + profile.name.capacity() + profile.items.capacity() * sizeof(int);
        }
    };

    struct StringSize
    {
        std::size_t operator()(const std::string &value) const
        {
            return sizeof(std::string) + value.capacity();
        }
    };
}

int main()
{
    using namespace yuan::game::server::cache;

    auto store = std::make_shared<MemoryCacheStore<std::uint64_t, PlayerProfile>>();
    store->put(1, PlayerProfile{1, "alice", {1, 2, 3}});

    CacheLimits limits;
    limits.max_entries = 3;
    limits.max_estimated_bytes = 1024 * 1024;
    limits.max_dirty_entries = 3;
    limits.idle_ttl = std::chrono::milliseconds{0};
    ObjectCache<std::uint64_t, PlayerProfile, std::hash<std::uint64_t>, std::equal_to<std::uint64_t>, PlayerProfileSize> profiles(limits, store, PlayerProfileSize{});

    auto loaded = profiles.get(1);
    if (!require(loaded.has_value(), "cache should load missing profile from store")) {
        return 1;
    }
    if (!require((*loaded).get().name == "alice", "loaded profile should match store value")) {
        return 2;
    }
    if (!require(profiles.stats().loads == 1 && profiles.stats().misses == 1, "load/miss stats should update")) {
        return 3;
    }

    auto hit = profiles.get(1);
    if (!require(hit.has_value(), "cache should hit existing profile")) {
        return 4;
    }
    if (!require(profiles.stats().hits == 1, "hit stats should update")) {
        return 5;
    }

    (*hit).get().items.push_back(4);
    (*hit).mark_dirty();
    if (!require(profiles.stats().dirty_entries == 1, "mark_dirty should update dirty count")) {
        return 6;
    }
    if (!require(profiles.flush(1), "dirty profile should flush")) {
        return 7;
    }
    if (!require(profiles.stats().dirty_entries == 0 && store->save_count() == 1, "flush should clear dirty state and save")) {
        return 8;
    }
    loaded.reset();
    hit.reset();

    auto bob = profiles.put(2, PlayerProfile{2, "bob", {7}}, false);
    auto carol = profiles.put(3, PlayerProfile{3, "carol", {8}}, false);
    if (!require(bob.has_value() && carol.has_value(), "put should insert clean profiles")) {
        return 9;
    }
    bob.reset();
    carol.reset();
    const auto evicted = profiles.evict_idle();
    if (!require(evicted == 3 && profiles.size() == 0, "idle clean profiles should evict")) {
        return 10;
    }

    CacheLimits small_limits;
    small_limits.max_entries = 1;
    small_limits.max_estimated_bytes = 4096;
    small_limits.idle_ttl = std::chrono::milliseconds{0};
    ObjectCache<int, std::string> strings(small_limits, {});
    auto first = strings.put(1, "first", false);
    if (!require(first.has_value(), "first string should insert")) {
        return 11;
    }
    auto second = strings.put(2, "second", false);
    if (!require(!second.has_value(), "pinned entry should prevent eviction and reject capacity overflow")) {
        return 12;
    }
    first.reset();
    second = strings.put(2, "second", false);
    if (!require(second.has_value(), "unpinned entry should be evicted for new entry")) {
        return 13;
    }

    CacheLimits byte_limits;
    byte_limits.max_entries = 10;
    byte_limits.max_estimated_bytes = sizeof(std::string);
    ObjectCache<int, std::string, std::hash<int>, std::equal_to<int>, StringSize> byte_cache(byte_limits, {}, StringSize{});
    auto rejected = byte_cache.put(1, std::string(1024, 'x'), false);
    if (!require(!rejected.has_value(), "cache should reject entries beyond byte limit")) {
        return 14;
    }
    if (!require(byte_cache.stats().rejected == 1, "rejection stats should update")) {
        return 15;
    }

    CacheLimits dirty_limits;
    dirty_limits.max_entries = 2;
    dirty_limits.max_estimated_bytes = 4096;
    dirty_limits.max_dirty_entries = 1;
    ObjectCache<int, std::string> dirty_cache(dirty_limits, {});
    auto dirty_one = dirty_cache.put(1, "dirty", true);
    auto dirty_two = dirty_cache.put(2, "also-dirty", true);
    if (!require(dirty_one.has_value() && !dirty_two.has_value(), "dirty limit should reject additional dirty inserts")) {
        return 16;
    }

    return 0;
}
