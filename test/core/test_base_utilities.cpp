#include "base/algorithm/bloom_filter.h"
#include "base/algorithm/consistent_hash.h"
#include "base/algorithm/scope_guard.h"
#include "base/algorithm/top_k.h"
#include "base/algorithm/wildcard_match.h"
#include "base/container/bounded_queue.h"
#include "base/container/lru_cache.h"
#include "base/container/recent_set.h"
#include "base/container/ring_buffer.h"
#include "base/container/ttl_cache.h"
#include "base/id/id_generator.h"
#include "base/id/snowflake_id_generator.h"
#include "base/rate/sliding_window_rate_limiter.h"
#include "base/rate/token_bucket.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    int failed = 0;

    void check(bool cond, const char *msg)
    {
        if (!cond) {
            ++failed;
            std::cerr << "[FAIL] " << msg << '\n';
        }
    }

    void test_scope_guard()
    {
        int value = 0;
        {
            auto guard = yuan::base::make_scope_exit([&] { value = 1; });
        }
        check(value == 1, "scope guard should run on scope exit");

        {
            auto guard = yuan::base::make_scope_exit([&] { value = 2; });
            guard.dismiss();
        }
        check(value == 1, "dismissed scope guard should not run");
    }

    void test_top_k()
    {
        yuan::base::TopK<int> top(3);
        for (int value : {5, 1, 9, 3, 7}) {
            top.push(value);
        }
        const auto values = top.sorted();
        check((values == std::vector<int>{9, 7, 5}), "top_k should keep best values");
    }

    void test_wildcard()
    {
        check(yuan::base::wildcard_match("*.example.com", "api.example.com"), "wildcard should match star");
        check(yuan::base::wildcard_match("file-?.txt", "file-a.txt"), "wildcard should match question mark");
        check(yuan::base::wildcard_match_ascii_ci("API-*", "api-test"), "wildcard should support ascii ci");
        check(!yuan::base::wildcard_match("a?c", "ac"), "wildcard question mark should require one char");
    }

    void test_bloom_filter()
    {
        yuan::base::BloomFilter<std::string> filter(1024, 4);
        filter.add("alpha");
        filter.add("beta");
        check(filter.possibly_contains("alpha"), "bloom should contain inserted key");
        check(filter.possibly_contains("beta"), "bloom should contain second inserted key");
        check(!filter.possibly_contains("definitely-not-added"), "bloom should reject clear miss in this test");
    }

    void test_consistent_hash()
    {
        yuan::base::ConsistentHash<std::string> hash(8);
        check(!hash.get_node("user").has_value(), "empty hash should return no node");
        hash.add_node("node-a", "node-a");
        hash.add_node("node-b", "node-b");
        const auto node = hash.get_node("user-1");
        check(node.has_value(), "consistent hash should find node");
        hash.remove_node("node-a");
        check(hash.ring_size() == 8, "consistent hash should remove virtual nodes");
    }

    void test_bounded_queue()
    {
        yuan::base::BoundedQueue<int> queue(2);
        queue.push(1);
        queue.push(2);
        queue.push(3);
        int value = 0;
        check(queue.dropped() == 1, "bounded queue should count dropped item");
        check(queue.pop(value) && value == 2, "bounded queue should drop oldest by default");
        check(queue.pop(value) && value == 3, "bounded queue should keep newest item");
    }

    void test_ring_buffer()
    {
        yuan::base::RingBuffer<int> ring(3);
        ring.push_back(1);
        ring.push_back(2);
        ring.push_back(3);
        ring.push_back(4);
        check(ring.size() == 3, "ring buffer should keep capacity size");
        check(ring[0] == 2 && ring[2] == 4, "ring buffer should overwrite oldest");
    }

    void test_lru_cache()
    {
        yuan::base::LruCache<std::string, int> cache(2);
        cache.put("a", 1);
        cache.put("b", 2);
        check(cache.get("a") && *cache.get("a") == 1, "lru should get existing value");
        cache.put("c", 3);
        check(cache.get("b") == nullptr, "lru should evict least recently used");
        check(cache.get("a") && cache.get("c"), "lru should keep recent values");
    }

    void test_ttl_cache()
    {
        using Clock = std::chrono::steady_clock;
        yuan::base::TtlCache<std::string, int, Clock> cache;
        const auto now = Clock::now();
        cache.put("a", 1, std::chrono::seconds(1), now);
        check(cache.get("a", now) && *cache.get("a", now) == 1, "ttl cache should return live value");
        check(cache.get("a", now + std::chrono::seconds(2)) == nullptr, "ttl cache should expire value");
    }

    void test_recent_set()
    {
        yuan::base::RecentSet<int> set(2);
        check(!set.seen_or_add(1), "recent set should mark new item as unseen");
        check(set.seen_or_add(1), "recent set should detect duplicate");
        set.seen_or_add(2);
        set.seen_or_add(3);
        check(!set.contains(1) && set.contains(3), "recent set should evict oldest item");
    }

    void test_id_generators()
    {
        yuan::base::AtomicIdGenerator<std::uint64_t> ids(0);
        check(ids.next() == 1, "atomic id generator should normalize zero start");
        check(ids.next() == 2, "atomic id generator should increment");

        yuan::base::SnowflakeIdGenerator snowflake(7);
        const auto t = std::chrono::milliseconds(1704067200000ULL + 1000);
        const auto a = snowflake.next(t);
        const auto b = snowflake.next(t);
        check(b > a, "snowflake id should increase within same millisecond");
    }

    void test_rate_limiters()
    {
        using Clock = std::chrono::steady_clock;
        const auto now = Clock::now();

        yuan::base::TokenBucket<Clock> bucket(1.0, 2.0, now);
        check(bucket.try_consume(2.0, now), "token bucket should allow burst");
        check(!bucket.try_consume(1.0, now), "token bucket should reject when empty");
        check(bucket.try_consume(1.0, now + std::chrono::seconds(1)), "token bucket should refill over time");

        yuan::base::SlidingWindowRateLimiter<std::string, Clock> limiter(2, std::chrono::seconds(10));
        check(limiter.allow("ip", now), "sliding limiter should allow first event");
        check(limiter.allow("ip", now + std::chrono::seconds(1)), "sliding limiter should allow second event");
        check(!limiter.allow("ip", now + std::chrono::seconds(2)), "sliding limiter should reject over limit");
        check(limiter.allow("ip", now + std::chrono::seconds(11)), "sliding limiter should allow after window");
    }
}

int main()
{
    test_scope_guard();
    test_top_k();
    test_wildcard();
    test_bloom_filter();
    test_consistent_hash();
    test_bounded_queue();
    test_ring_buffer();
    test_lru_cache();
    test_ttl_cache();
    test_recent_set();
    test_id_generators();
    test_rate_limiters();

    if (failed != 0) {
        std::cerr << "base_utilities failed=" << failed << '\n';
        return 1;
    }

    std::cout << "base_utilities passed\n";
    return 0;
}
