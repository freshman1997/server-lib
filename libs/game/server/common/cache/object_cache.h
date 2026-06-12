#ifndef YUAN_GAME_SERVER_COMMON_CACHE_OBJECT_CACHE_H
#define YUAN_GAME_SERVER_COMMON_CACHE_OBJECT_CACHE_H

#include "common/cache/cache_store.h"
#include "common/cache/cache_types.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace yuan::game::server::cache
{
    template <class Value>
    struct CacheSizeEstimator
    {
        std::size_t operator()(const Value &) const
        {
            return sizeof(Value);
        }
    };

    template <class Key, class Value>
    struct CacheEntry
    {
        Key key;
        Value value;
        CacheEntryState state = CacheEntryState::ready;
        std::uint64_t version = 0;
        std::size_t estimated_bytes = 0;
        std::size_t pin_count = 0;
        std::chrono::steady_clock::time_point last_access = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_mutation = std::chrono::steady_clock::now();
    };

    template <class Key, class Value, class Hash = std::hash<Key>, class Equal = std::equal_to<Key>, class SizeEstimator = CacheSizeEstimator<Value>>
    class ObjectCache;

    template <class Key, class Value, class Hash = std::hash<Key>, class Equal = std::equal_to<Key>, class SizeEstimator = CacheSizeEstimator<Value>>
    class CacheHandle
    {
    public:
        using Cache = ObjectCache<Key, Value, Hash, Equal, SizeEstimator>;
        using Entry = CacheEntry<Key, Value>;

        CacheHandle() = default;
        CacheHandle(Cache *cache, std::shared_ptr<Entry> entry)
            : cache_(cache), entry_(std::move(entry))
        {
        }

        CacheHandle(const CacheHandle &) = delete;
        CacheHandle &operator=(const CacheHandle &) = delete;

        CacheHandle(CacheHandle &&other) noexcept
            : cache_(other.cache_), entry_(std::move(other.entry_))
        {
            other.cache_ = nullptr;
        }

        CacheHandle &operator=(CacheHandle &&other) noexcept
        {
            if (this != &other) {
                release();
                cache_ = other.cache_;
                entry_ = std::move(other.entry_);
                other.cache_ = nullptr;
            }
            return *this;
        }

        ~CacheHandle()
        {
            release();
        }

        [[nodiscard]] explicit operator bool() const
        {
            return static_cast<bool>(entry_);
        }

        Value &get()
        {
            if (!entry_) {
                throw std::logic_error("invalid cache handle");
            }
            return entry_->value;
        }

        const Value &get() const
        {
            if (!entry_) {
                throw std::logic_error("invalid cache handle");
            }
            return entry_->value;
        }

        const Key &key() const
        {
            if (!entry_) {
                throw std::logic_error("invalid cache handle");
            }
            return entry_->key;
        }

        void mark_dirty();

    private:
        void release()
        {
            if (cache_ && entry_) {
                cache_->release_pin(entry_);
            }
            cache_ = nullptr;
            entry_.reset();
        }

        Cache *cache_ = nullptr;
        std::shared_ptr<Entry> entry_;
    };

    template <class Key, class Value, class Hash, class Equal, class SizeEstimator>
    class ObjectCache
    {
    public:
        using Store = ICacheStore<Key, Value>;
        using Entry = CacheEntry<Key, Value>;
        using Handle = CacheHandle<Key, Value, Hash, Equal, SizeEstimator>;

        explicit ObjectCache(CacheLimits limits = {}, std::shared_ptr<Store> store = {}, SizeEstimator estimator = {})
            : limits_(limits), store_(std::move(store)), estimator_(std::move(estimator))
        {
            if (limits_.low_watermark > limits_.high_watermark) {
                limits_.low_watermark = limits_.high_watermark;
            }
        }

        std::optional<Handle> get(const Key &key)
        {
            const auto now = std::chrono::steady_clock::now();
            if (auto hit = find_entry(key, now)) {
                ++stats_.hits;
                pin(*hit);
                return Handle(this, *hit);
            }

            ++stats_.misses;
            if (!store_) {
                return std::nullopt;
            }
            if (!reserve_for_new_entry(limits_.default_entry_bytes)) {
                ++stats_.rejected;
                return std::nullopt;
            }

            auto loaded = store_->load(key);
            if (loaded.status != CacheStoreStatus::ok || !loaded.value) {
                return std::nullopt;
            }
            ++stats_.loads;
            return insert_loaded(key, std::move(*loaded.value), now);
        }

        std::optional<Handle> put(Key key, Value value, bool dirty = true)
        {
            const auto now = std::chrono::steady_clock::now();
            const auto bytes = estimator_(value);
            if (!reserve_for_new_entry(bytes)) {
                ++stats_.rejected;
                return std::nullopt;
            }

            std::shared_ptr<Entry> entry;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto existing = entries_.find(key);
                if (existing != entries_.end()) {
                    entry = existing->second;
                    adjust_bytes_locked(entry->estimated_bytes, bytes);
                    if (entry->state == CacheEntryState::dirty && !dirty) {
                        --stats_.dirty_entries;
                    } else if (entry->state != CacheEntryState::dirty && dirty) {
                        ++stats_.dirty_entries;
                    }
                    entry->value = std::move(value);
                    entry->estimated_bytes = bytes;
                    entry->state = dirty ? CacheEntryState::dirty : CacheEntryState::ready;
                    entry->last_access = now;
                    entry->last_mutation = now;
                } else {
                    entry = std::make_shared<Entry>();
                    entry->key = std::move(key);
                    entry->value = std::move(value);
                    entry->estimated_bytes = bytes;
                    entry->state = dirty ? CacheEntryState::dirty : CacheEntryState::ready;
                    entry->last_access = now;
                    entry->last_mutation = now;
                    entries_[entry->key] = entry;
                    ++stats_.entries;
                    stats_.estimated_bytes += bytes;
                    if (dirty) {
                        ++stats_.dirty_entries;
                    }
                }
                pin_locked(*entry);
            }
            return Handle(this, entry);
        }

        bool contains(const Key &key) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return entries_.find(key) != entries_.end();
        }

        bool mark_dirty(const Key &key)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(key);
            if (it == entries_.end()) {
                return false;
            }
            return mark_dirty_locked(*it->second);
        }

        bool flush(const Key &key)
        {
            std::shared_ptr<Entry> entry;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = entries_.find(key);
                if (it == entries_.end() || it->second->state != CacheEntryState::dirty) {
                    return false;
                }
                entry = it->second;
                entry->state = CacheEntryState::flushing;
            }

            const auto status = store_ ? store_->save(entry->key, entry->value, entry->version) : CacheStoreStatus::error;

            std::lock_guard<std::mutex> lock(mutex_);
            if (status == CacheStoreStatus::ok) {
                entry->state = CacheEntryState::ready;
                if (stats_.dirty_entries > 0) {
                    --stats_.dirty_entries;
                }
                ++stats_.saves;
                return true;
            }
            entry->state = CacheEntryState::dirty;
            return false;
        }

        std::size_t flush_all()
        {
            std::vector<Key> dirty_keys;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                dirty_keys.reserve(stats_.dirty_entries);
                for (const auto &[key, entry] : entries_) {
                    if (entry->state == CacheEntryState::dirty) {
                        dirty_keys.push_back(key);
                    }
                }
            }

            std::size_t flushed = 0;
            for (const auto &key : dirty_keys) {
                if (flush(key)) {
                    ++flushed;
                }
            }
            return flushed;
        }

        bool erase(const Key &key)
        {
            std::shared_ptr<Entry> entry;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = entries_.find(key);
                if (it == entries_.end() || it->second->pin_count != 0 || it->second->state == CacheEntryState::flushing) {
                    return false;
                }
                entry = it->second;
            }
            if (entry->state == CacheEntryState::dirty && limits_.flush_dirty_on_evict && !flush(key)) {
                return false;
            }
            return erase_clean(key);
        }

        std::size_t evict_idle()
        {
            const auto now = std::chrono::steady_clock::now();
            std::vector<Key> candidates;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto &[key, entry] : entries_) {
                    if (entry->pin_count == 0 && entry->state == CacheEntryState::ready && now - entry->last_access >= limits_.idle_ttl) {
                        candidates.push_back(key);
                    }
                }
            }

            std::size_t evicted = 0;
            for (const auto &key : candidates) {
                if (erase_clean(key)) {
                    ++evicted;
                }
            }
            return evicted;
        }

        std::size_t evict_to_low_watermark()
        {
            std::vector<std::shared_ptr<Entry>> candidates;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto &[_, entry] : entries_) {
                    if (entry->pin_count == 0 && entry->state == CacheEntryState::ready) {
                        candidates.push_back(entry);
                    }
                }
            }
            std::sort(candidates.begin(), candidates.end(), [](const auto &lhs, const auto &rhs) {
                return lhs->last_access < rhs->last_access;
            });

            std::size_t evicted = 0;
            for (const auto &entry : candidates) {
                if (below_low_watermark()) {
                    break;
                }
                if (erase_clean(entry->key)) {
                    ++evicted;
                }
            }
            return evicted;
        }

        template <class Fn>
        bool with_value(const Key &key, Fn &&fn, bool dirty = true)
        {
            auto handle = get(key);
            if (!handle) {
                return false;
            }
            std::forward<Fn>(fn)((*handle).get());
            if (dirty) {
                handle->mark_dirty();
            }
            return true;
        }

        [[nodiscard]] CacheStats stats() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return stats_;
        }

        [[nodiscard]] CachePressure pressure() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return pressure_locked();
        }

        [[nodiscard]] std::size_t size() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return entries_.size();
        }

    private:
        friend class CacheHandle<Key, Value, Hash, Equal, SizeEstimator>;

        std::optional<std::shared_ptr<Entry>> find_entry(const Key &key, std::chrono::steady_clock::time_point now)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(key);
            if (it == entries_.end()) {
                return std::nullopt;
            }
            it->second->last_access = now;
            return it->second;
        }

        std::optional<Handle> insert_loaded(const Key &key, Value value, std::chrono::steady_clock::time_point now)
        {
            const auto bytes = estimator_(value);
            std::shared_ptr<Entry> entry;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto existing = entries_.find(key);
                if (existing != entries_.end()) {
                    entry = existing->second;
                    adjust_bytes_locked(entry->estimated_bytes, bytes);
                    entry->value = std::move(value);
                    entry->estimated_bytes = bytes;
                    entry->last_access = now;
                    entry->state = CacheEntryState::ready;
                } else {
                    entry = std::make_shared<Entry>();
                    entry->key = key;
                    entry->value = std::move(value);
                    entry->estimated_bytes = bytes;
                    entry->last_access = now;
                    entries_[key] = entry;
                    ++stats_.entries;
                    stats_.estimated_bytes += bytes;
                }
                pin_locked(*entry);
            }
            return Handle(this, entry);
        }

        bool reserve_for_new_entry(std::size_t incoming_bytes)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (can_admit_locked(incoming_bytes)) {
                    return true;
                }
            }
            evict_to_low_watermark();
            std::lock_guard<std::mutex> lock(mutex_);
            return can_admit_locked(incoming_bytes);
        }

        bool can_admit_locked(std::size_t incoming_bytes) const
        {
            if (stats_.entries + 1 > limits_.max_entries) {
                return false;
            }
            if (stats_.estimated_bytes + incoming_bytes > limits_.max_estimated_bytes) {
                return false;
            }
            if (stats_.dirty_entries >= limits_.max_dirty_entries) {
                return false;
            }
            return true;
        }

        void pin(const std::shared_ptr<Entry> &entry)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pin_locked(*entry);
        }

        void pin_locked(Entry &entry)
        {
            if (entry.pin_count == 0) {
                ++stats_.pinned_entries;
            }
            ++entry.pin_count;
        }

        void release_pin(const std::shared_ptr<Entry> &entry)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (entry->pin_count == 0) {
                return;
            }
            --entry->pin_count;
            if (entry->pin_count == 0 && stats_.pinned_entries > 0) {
                --stats_.pinned_entries;
            }
        }

        void mark_dirty_from_handle(const std::shared_ptr<Entry> &entry)
        {
            const auto bytes = estimator_(entry->value);
            std::lock_guard<std::mutex> lock(mutex_);
            adjust_bytes_locked(entry->estimated_bytes, bytes);
            entry->estimated_bytes = bytes;
            entry->last_mutation = std::chrono::steady_clock::now();
            ++entry->version;
            (void)mark_dirty_locked(*entry);
        }

        bool mark_dirty_locked(Entry &entry)
        {
            if (entry.state == CacheEntryState::dirty) {
                return true;
            }
            if (stats_.dirty_entries >= limits_.max_dirty_entries) {
                return false;
            }
            entry.state = CacheEntryState::dirty;
            entry.last_mutation = std::chrono::steady_clock::now();
            ++stats_.dirty_entries;
            return true;
        }

        bool erase_clean(const Key &key)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(key);
            if (it == entries_.end() || it->second->pin_count != 0 || it->second->state != CacheEntryState::ready) {
                return false;
            }
            stats_.estimated_bytes -= std::min(stats_.estimated_bytes, it->second->estimated_bytes);
            --stats_.entries;
            ++stats_.evictions;
            entries_.erase(it);
            return true;
        }

        void adjust_bytes_locked(std::size_t old_bytes, std::size_t new_bytes)
        {
            if (new_bytes >= old_bytes) {
                stats_.estimated_bytes += new_bytes - old_bytes;
            } else {
                stats_.estimated_bytes -= std::min(stats_.estimated_bytes, old_bytes - new_bytes);
            }
        }

        bool below_low_watermark() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return below_low_watermark_locked();
        }

        bool below_low_watermark_locked() const
        {
            const auto max_entries = std::max<std::size_t>(limits_.max_entries, 1);
            const auto max_bytes = std::max<std::size_t>(limits_.max_estimated_bytes, 1);
            const double entry_ratio = static_cast<double>(stats_.entries) / static_cast<double>(max_entries);
            const double byte_ratio = static_cast<double>(stats_.estimated_bytes) / static_cast<double>(max_bytes);
            return std::max(entry_ratio, byte_ratio) <= limits_.low_watermark;
        }

        CachePressure pressure_locked() const
        {
            const auto max_entries = std::max<std::size_t>(limits_.max_entries, 1);
            const auto max_bytes = std::max<std::size_t>(limits_.max_estimated_bytes, 1);
            const double entry_ratio = static_cast<double>(stats_.entries) / static_cast<double>(max_entries);
            const double byte_ratio = static_cast<double>(stats_.estimated_bytes) / static_cast<double>(max_bytes);
            const double ratio = std::max(entry_ratio, byte_ratio);
            if (ratio >= 1.0) {
                return CachePressure::critical;
            }
            if (ratio >= limits_.high_watermark) {
                return CachePressure::high;
            }
            if (ratio >= limits_.low_watermark) {
                return CachePressure::elevated;
            }
            return CachePressure::normal;
        }

        CacheLimits limits_;
        std::shared_ptr<Store> store_;
        SizeEstimator estimator_;
        mutable std::mutex mutex_;
        std::unordered_map<Key, std::shared_ptr<Entry>, Hash, Equal> entries_;
        CacheStats stats_;
    };

    template <class Key, class Value, class Hash, class Equal, class SizeEstimator>
    void CacheHandle<Key, Value, Hash, Equal, SizeEstimator>::mark_dirty()
    {
        if (!cache_ || !entry_) {
            return;
        }
        cache_->mark_dirty_from_handle(entry_);
    }
}

#endif
