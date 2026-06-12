#ifndef YUAN_BASE_CONTAINER_TTL_CACHE_H_
#define YUAN_BASE_CONTAINER_TTL_CACHE_H_

#include <chrono>
#include <unordered_map>
#include <utility>

namespace yuan::base
{
    // TtlCache 是带过期时间的简单缓存。读取过期条目时会自动删除，也可以主动 prune。
    //
    // 适用场景：临时 token、握手状态、防重放 nonce、DNS/配置短期缓存。
    // 用法：
    //   yuan::base::TtlCache<std::string, int> cache;
    //   cache.put("a", 1, std::chrono::seconds(5));
    //   auto *v = cache.get("a");
    template <typename Key, typename Value, typename Clock = std::chrono::steady_clock,
              typename Hash = std::hash<Key>, typename Equal = std::equal_to<Key>>
    class TtlCache
    {
    public:
        using TimePoint = typename Clock::time_point;
        using Duration = typename Clock::duration;

        void put(Key key, Value value, Duration ttl, TimePoint now = Clock::now())
        {
            entries_[std::move(key)] = Entry{std::move(value), now + ttl};
        }

        Value* get(const Key &key, TimePoint now = Clock::now())
        {
            auto it = entries_.find(key);
            if (it == entries_.end()) {
                return nullptr;
            }
            if (now >= it->second.expires_at) {
                entries_.erase(it);
                return nullptr;
            }
            return &it->second.value;
        }

        bool erase(const Key &key)
        {
            return entries_.erase(key) > 0;
        }

        void prune(TimePoint now = Clock::now())
        {
            for (auto it = entries_.begin(); it != entries_.end();) {
                if (now >= it->second.expires_at) {
                    it = entries_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        void clear()
        {
            entries_.clear();
        }

        std::size_t size() const noexcept { return entries_.size(); }
        bool empty() const noexcept { return entries_.empty(); }

    private:
        struct Entry
        {
            Value value;
            TimePoint expires_at;
        };

        std::unordered_map<Key, Entry, Hash, Equal> entries_;
    };
}

#endif // YUAN_BASE_CONTAINER_TTL_CACHE_H_
