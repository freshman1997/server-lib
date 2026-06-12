#ifndef YUAN_GAME_SERVER_COMMON_CACHE_CACHE_STORE_H
#define YUAN_GAME_SERVER_COMMON_CACHE_CACHE_STORE_H

#include "common/cache/cache_types.h"

#include <optional>
#include <unordered_map>
#include <utility>

namespace yuan::game::server::cache
{
    template <class Value>
    struct CacheLoadResult
    {
        CacheStoreStatus status = CacheStoreStatus::not_found;
        std::optional<Value> value;
    };

    template <class Key, class Value>
    class ICacheStore
    {
    public:
        virtual ~ICacheStore() = default;

        virtual CacheLoadResult<Value> load(const Key &key) = 0;
        virtual CacheStoreStatus save(const Key &key, const Value &value, std::uint64_t version) = 0;
        virtual CacheStoreStatus remove(const Key &key) = 0;
    };

    template <class Key, class Value, class Hash = std::hash<Key>, class Equal = std::equal_to<Key>>
    class MemoryCacheStore final : public ICacheStore<Key, Value>
    {
    public:
        CacheLoadResult<Value> load(const Key &key) override
        {
            const auto it = values_.find(key);
            if (it == values_.end()) {
                return {CacheStoreStatus::not_found, std::nullopt};
            }
            return {CacheStoreStatus::ok, it->second};
        }

        CacheStoreStatus save(const Key &key, const Value &value, std::uint64_t) override
        {
            values_[key] = value;
            ++save_count_;
            return CacheStoreStatus::ok;
        }

        CacheStoreStatus remove(const Key &key) override
        {
            values_.erase(key);
            return CacheStoreStatus::ok;
        }

        void put(Key key, Value value)
        {
            values_[std::move(key)] = std::move(value);
        }

        [[nodiscard]] bool contains(const Key &key) const
        {
            return values_.find(key) != values_.end();
        }

        [[nodiscard]] std::size_t size() const
        {
            return values_.size();
        }

        [[nodiscard]] std::uint64_t save_count() const
        {
            return save_count_;
        }

    private:
        std::unordered_map<Key, Value, Hash, Equal> values_;
        std::uint64_t save_count_ = 0;
    };
}

#endif
