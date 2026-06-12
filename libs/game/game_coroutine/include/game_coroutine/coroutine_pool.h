#ifndef __YUAN_GAME_COROUTINE_COROUTINE_POOL_H__
#define __YUAN_GAME_COROUTINE_COROUTINE_POOL_H__

#include <any>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace yuan::game_coroutine
{
    using CoroutineId = std::uint64_t;

    enum class CoroutineWakeReason
    {
        completed,
        timed_out,
        canceled,
        failed
    };

    using CoroutineContextValue = std::any;
    using CoroutineContextMap = std::unordered_map<std::string, CoroutineContextValue>;

    struct CoroutineWakeResult
    {
        CoroutineId id = 0;
        CoroutineWakeReason reason = CoroutineWakeReason::completed;
        std::string payload;
        std::string error;
        CoroutineContextMap context;

        template<typename T>
        const T *context_as(std::string_view key) const
        {
            const auto it = context.find(std::string(key));
            if (it == context.end()) {
                return nullptr;
            }
            return std::any_cast<T>(&it->second);
        }
    };

    using CoroutineWakeCallback = std::function<void(const CoroutineWakeResult &)>;

    struct PendingCoroutinePoolStats
    {
        std::size_t pending = 0;
        std::uint64_t created_total = 0;
        std::uint64_t wake_completed_total = 0;
        std::uint64_t wake_failed_total = 0;
        std::uint64_t wake_canceled_total = 0;
        std::uint64_t wake_timed_out_total = 0;
        std::uint64_t wake_not_found_total = 0;
    };

    struct PendingCoroutineSnapshotEntry
    {
        CoroutineId id = 0;
        std::optional<std::int64_t> deadline_unix_ms;
        CoroutineContextMap context;
    };

    class PendingCoroutinePool
    {
    public:
        using Clock = std::chrono::steady_clock;
        using Duration = Clock::duration;
        using SystemClock = std::chrono::system_clock;
        using CancelPredicate = std::function<bool(CoroutineId, const CoroutineContextMap &)>;

        PendingCoroutinePool() = default;

        CoroutineId create(CoroutineWakeCallback callback,
                           std::optional<Duration> timeout = std::nullopt)
        {
            CoroutineId id = next_id_.fetch_add(1, std::memory_order_relaxed);
            while (id == 0) {
                id = next_id_.fetch_add(1, std::memory_order_relaxed);
            }
            while (!create_with_id(id, std::move(callback), timeout)) {
                id = next_id_.fetch_add(1, std::memory_order_relaxed);
                if (id == 0) {
                    id = next_id_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return id;
        }

        bool create_with_id(CoroutineId id,
                            CoroutineWakeCallback callback,
                            std::optional<Duration> timeout = std::nullopt)
        {
            if (id == 0 || !callback) {
                return false;
            }

            Entry entry;
            entry.callback = std::move(callback);
            if (timeout.has_value() && *timeout > Duration::zero()) {
                entry.deadline = Clock::now() + *timeout;
                entry.deadline_system = SystemClock::now() +
                    std::chrono::duration_cast<SystemClock::duration>(*timeout);
            }

            std::lock_guard<std::mutex> lock(mutex_);
            auto [it, inserted] = entries_.emplace(id, std::move(entry));
            (void)it;
            if (inserted) {
                stats_.created_total++;
            }
            return inserted;
        }

        bool restore_from_snapshot(const PendingCoroutineSnapshotEntry &snapshot,
                                   CoroutineWakeCallback callback)
        {
            if (snapshot.id == 0 || !callback) {
                return false;
            }

            Entry entry;
            entry.callback = std::move(callback);
            entry.context = snapshot.context;
            entry.deadline_system = to_system_deadline(snapshot.deadline_unix_ms);
            if (entry.deadline_system.has_value()) {
                const auto now_system = SystemClock::now();
                const auto now_steady = Clock::now();
                if (*entry.deadline_system <= now_system) {
                    entry.deadline = now_steady;
                } else {
                    const auto remaining = *entry.deadline_system - now_system;
                    entry.deadline = now_steady +
                        std::chrono::duration_cast<Clock::duration>(remaining);
                }
            }

            std::lock_guard<std::mutex> lock(mutex_);
            auto [it, inserted] = entries_.emplace(snapshot.id, std::move(entry));
            (void)it;
            if (inserted) {
                stats_.created_total++;
            }
            return inserted;
        }

        std::vector<PendingCoroutineSnapshotEntry> snapshot_entries() const
        {
            std::vector<PendingCoroutineSnapshotEntry> snapshots;
            std::lock_guard<std::mutex> lock(mutex_);
            snapshots.reserve(entries_.size());
            for (const auto &[id, entry] : entries_) {
                PendingCoroutineSnapshotEntry snapshot;
                snapshot.id = id;
                snapshot.deadline_unix_ms = to_unix_ms(entry.deadline_system);
                snapshot.context = entry.context;
                snapshots.push_back(std::move(snapshot));
            }
            return snapshots;
        }

        template<typename T>
        bool set_context(CoroutineId id, std::string key, T &&value)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(id);
            if (it == entries_.end()) {
                return false;
            }
            it->second.context[std::move(key)] = make_context_value(std::forward<T>(value));
            return true;
        }

        bool set_context_any(CoroutineId id, std::string key, CoroutineContextValue value)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(id);
            if (it == entries_.end()) {
                return false;
            }
            it->second.context[std::move(key)] = std::move(value);
            return true;
        }

        template<typename T>
        std::optional<T> get_context(CoroutineId id, std::string_view key) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(id);
            if (it == entries_.end()) {
                return std::nullopt;
            }
            const auto key_it = it->second.context.find(std::string(key));
            if (key_it == it->second.context.end()) {
                return std::nullopt;
            }
            const T *value = std::any_cast<T>(&key_it->second);
            if (!value) {
                return std::nullopt;
            }
            return *value;
        }

        std::optional<CoroutineContextValue> get_context_any(CoroutineId id, std::string_view key) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(id);
            if (it == entries_.end()) {
                return std::nullopt;
            }
            const auto key_it = it->second.context.find(std::string(key));
            if (key_it == it->second.context.end()) {
                return std::nullopt;
            }
            return key_it->second;
        }

        std::optional<CoroutineContextMap> get_context_map(CoroutineId id) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(id);
            if (it == entries_.end()) {
                return std::nullopt;
            }
            return it->second.context;
        }

        bool wake_completed(CoroutineId id, std::string payload = {})
        {
            return wake(id, CoroutineWakeReason::completed, std::move(payload), {});
        }

        bool wake_failed(CoroutineId id, std::string error)
        {
            return wake(id, CoroutineWakeReason::failed, {}, std::move(error));
        }

        bool wake_canceled(CoroutineId id, std::string error = "canceled")
        {
            return wake(id, CoroutineWakeReason::canceled, {}, std::move(error));
        }

        std::size_t cancel_where(CancelPredicate predicate, std::string error = "canceled")
        {
            if (!predicate) {
                return 0;
            }

            std::vector<std::pair<CoroutineWakeCallback, CoroutineWakeResult>> wakeups;
            wakeups.reserve(8);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto it = entries_.begin(); it != entries_.end();) {
                    if (!predicate(it->first, it->second.context)) {
                        ++it;
                        continue;
                    }

                    CoroutineWakeResult result;
                    result.id = it->first;
                    result.reason = CoroutineWakeReason::canceled;
                    result.error = error;
                    result.context = std::move(it->second.context);
                    wakeups.emplace_back(std::move(it->second.callback), std::move(result));
                    it = entries_.erase(it);
                }
                stats_.wake_canceled_total += wakeups.size();
            }

            for (auto &wake : wakeups) {
                wake.first(wake.second);
            }
            return wakeups.size();
        }

        std::size_t cancel_by_tag(std::string_view tag, std::string error = "canceled")
        {
            return cancel_where(
                [tag](CoroutineId, const CoroutineContextMap &context) {
                    const auto it = context.find("tag");
                    if (it == context.end()) {
                        return false;
                    }
                    const std::string *tag_value = std::any_cast<std::string>(&it->second);
                    return tag_value && *tag_value == tag;
                },
                std::move(error));
        }

        std::size_t wake_timeouts(Clock::time_point now = Clock::now(),
                                  std::size_t limit = (std::numeric_limits<std::size_t>::max)())
        {
            std::vector<std::pair<CoroutineWakeCallback, CoroutineWakeResult>> wakeups;
            wakeups.reserve(8);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (limit == 0) {
                    return 0;
                }
                for (auto it = entries_.begin(); it != entries_.end() && wakeups.size() < limit;) {
                    if (!it->second.deadline.has_value() || now < *it->second.deadline) {
                        ++it;
                        continue;
                    }

                    CoroutineWakeResult result;
                    result.id = it->first;
                    result.reason = CoroutineWakeReason::timed_out;
                    result.error = "timed out";
                    result.context = std::move(it->second.context);
                    wakeups.emplace_back(std::move(it->second.callback), std::move(result));
                    it = entries_.erase(it);
                }
                stats_.wake_timed_out_total += wakeups.size();
            }

            for (auto &wake : wakeups) {
                wake.first(wake.second);
            }
            return wakeups.size();
        }

        bool contains(CoroutineId id) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return entries_.find(id) != entries_.end();
        }

        std::size_t size() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return entries_.size();
        }

        PendingCoroutinePoolStats stats_snapshot() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            PendingCoroutinePoolStats snapshot = stats_;
            snapshot.pending = entries_.size();
            return snapshot;
        }

    private:
        template<typename T>
        static std::any make_context_value(T &&value)
        {
            using ValueType = std::decay_t<T>;
            if constexpr (std::is_same_v<ValueType, const char *> || std::is_same_v<ValueType, char *>) {
                return std::string(value ? value : "");
            } else if constexpr (std::is_same_v<ValueType, std::string_view>) {
                return std::string(value);
            } else if constexpr (std::is_same_v<ValueType, std::any>) {
                return std::forward<T>(value);
            } else {
                return std::any(std::forward<T>(value));
            }
        }

        struct Entry
        {
            CoroutineWakeCallback callback;
            std::optional<Clock::time_point> deadline;
            std::optional<SystemClock::time_point> deadline_system;
            CoroutineContextMap context;
        };

        static std::optional<std::int64_t> to_unix_ms(const std::optional<SystemClock::time_point> &tp)
        {
            if (!tp.has_value()) {
                return std::nullopt;
            }
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       tp->time_since_epoch())
                .count();
        }

        static std::optional<SystemClock::time_point> to_system_deadline(const std::optional<std::int64_t> &unix_ms)
        {
            if (!unix_ms.has_value()) {
                return std::nullopt;
            }
            return SystemClock::time_point{std::chrono::milliseconds(*unix_ms)};
        }

        bool wake(CoroutineId id,
                  CoroutineWakeReason reason,
                  std::string payload,
                  std::string error)
        {
            std::pair<CoroutineWakeCallback, CoroutineWakeResult> wakeup;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = entries_.find(id);
                if (it == entries_.end()) {
                    stats_.wake_not_found_total++;
                    return false;
                }

                wakeup.first = std::move(it->second.callback);
                wakeup.second.id = id;
                wakeup.second.reason = reason;
                wakeup.second.payload = std::move(payload);
                wakeup.second.error = std::move(error);
                wakeup.second.context = std::move(it->second.context);
                entries_.erase(it);

                switch (reason) {
                case CoroutineWakeReason::completed:
                    stats_.wake_completed_total++;
                    break;
                case CoroutineWakeReason::timed_out:
                    stats_.wake_timed_out_total++;
                    break;
                case CoroutineWakeReason::canceled:
                    stats_.wake_canceled_total++;
                    break;
                case CoroutineWakeReason::failed:
                    stats_.wake_failed_total++;
                    break;
                }
            }

            wakeup.first(wakeup.second);
            return true;
        }

        mutable std::mutex mutex_;
        std::unordered_map<CoroutineId, Entry> entries_;
        std::atomic<CoroutineId> next_id_{1};
        PendingCoroutinePoolStats stats_;
    };
}

#endif
