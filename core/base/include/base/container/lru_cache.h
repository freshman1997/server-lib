#ifndef YUAN_BASE_CONTAINER_LRU_CACHE_H_
#define YUAN_BASE_CONTAINER_LRU_CACHE_H_

#include <cstddef>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>

namespace yuan::base
{
    // LruCache 是固定容量最近最少使用缓存。访问或写入会把条目移动到最新位置，
    // 超过容量时淘汰最久未使用条目。
    //
    // 适用场景：DNS/路由/配置/文件元信息/临时计算结果缓存。
    // 用法：
    //   yuan::base::LruCache<std::string, int> cache(128);
    //   cache.put("a", 1);
    //   if (auto *v = cache.get("a")) { ... }
    template <typename Key, typename Value, typename Hash = std::hash<Key>, typename Equal = std::equal_to<Key>>
    class LruCache
    {
    public:
        explicit LruCache(std::size_t capacity)
            : capacity_(capacity)
        {
        }

        void put(Key key, Value value)
        {
            if (capacity_ == 0) {
                return;
            }

            auto it = index_.find(key);
            if (it != index_.end()) {
                it->second->value = std::move(value);
                touch(it->second);
                return;
            }

            items_.push_front(Entry{std::move(key), std::move(value)});
            index_[items_.front().key] = items_.begin();

            if (items_.size() > capacity_) {
                index_.erase(items_.back().key);
                items_.pop_back();
            }
        }

        Value* get(const Key &key)
        {
            auto it = index_.find(key);
            if (it == index_.end()) {
                return nullptr;
            }
            touch(it->second);
            return &items_.front().value;
        }

        const Value* get(const Key &key) const
        {
            auto it = index_.find(key);
            return it == index_.end() ? nullptr : &it->second->value;
        }

        std::optional<Value> get_copy(const Key &key)
        {
            Value *value = get(key);
            if (!value) {
                return std::nullopt;
            }
            return *value;
        }

        bool erase(const Key &key)
        {
            auto it = index_.find(key);
            if (it == index_.end()) {
                return false;
            }
            items_.erase(it->second);
            index_.erase(it);
            return true;
        }

        void clear()
        {
            items_.clear();
            index_.clear();
        }

        std::size_t size() const noexcept { return items_.size(); }
        std::size_t capacity() const noexcept { return capacity_; }
        bool empty() const noexcept { return items_.empty(); }

    private:
        struct Entry
        {
            Key key;
            Value value;
        };

        using List = std::list<Entry>;
        using Iterator = typename List::iterator;

        void touch(Iterator it)
        {
            if (it != items_.begin()) {
                items_.splice(items_.begin(), items_, it);
            }
        }

        std::size_t capacity_ = 0;
        List items_;
        std::unordered_map<Key, Iterator, Hash, Equal> index_;
    };
}

#endif // YUAN_BASE_CONTAINER_LRU_CACHE_H_
