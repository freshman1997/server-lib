#ifndef __YUAN_GAME_COROUTINE_PENDING_RPC_SERVICE_H__
#define __YUAN_GAME_COROUTINE_PENDING_RPC_SERVICE_H__

#include "game_coroutine/coroutine_pool.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yuan::game_coroutine
{
    // PendingRpcService is a lightweight runtime helper for request/response style
    // coroutine orchestration inside one process.
    //
    // Typical flow:
    // 1) begin_request(route, on_wake, timeout)
    // 2) attach request context with set_request_context(...)
    // 3) when remote response returns, call complete/fail/cancel by coroutine id
    // 4) call tick() in service loop to wake timed-out requests
    //
    // Example:
    //   PendingRpcService svc;
    //   auto id = svc.begin_request("inventory.commit", [](const CoroutineWakeResult &r) {
    //       if (r.reason == CoroutineWakeReason::completed) {
    //           // deserialize r.payload by business convention
    //       }
    //   }, std::chrono::seconds(3));
    //   svc.set_request_context(id, "trace_id", std::string("abc"));
    //   // ... later from transport callback:
    //   svc.complete(id, "{\"ok\":true}");
    //   // ... periodically:
    //   svc.tick();
    class PendingRpcService
    {
    public:
        using Clock = PendingCoroutinePool::Clock;
        using Duration = PendingCoroutinePool::Duration;
        using OnWake = CoroutineWakeCallback;

        struct SnapshotItem
        {
            CoroutineId id = 0;
            std::optional<std::int64_t> deadline_unix_ms;
            CoroutineContextMap context;
        };

        CoroutineId begin_request(std::string route,
                                  OnWake on_wake,
                                  std::optional<Duration> timeout = std::nullopt)
        {
            const CoroutineId id = pool_.create(std::move(on_wake), timeout);
            (void)pool_.set_context(id, "route", std::move(route));
            return id;
        }

        bool begin_request_with_id(CoroutineId id,
                                   std::string route,
                                   OnWake on_wake,
                                   std::optional<Duration> timeout = std::nullopt)
        {
            if (!pool_.create_with_id(id, std::move(on_wake), timeout)) {
                return false;
            }
            (void)pool_.set_context(id, "route", std::move(route));
            return true;
        }

        CoroutineId begin_request_for_owner(std::string owner,
                                            std::string route,
                                            OnWake on_wake,
                                            std::optional<Duration> timeout = std::nullopt)
        {
            const CoroutineId id = begin_request(std::move(route), std::move(on_wake), timeout);
            (void)pool_.set_context(id, "owner", std::move(owner));
            return id;
        }

        template<typename T>
        std::optional<T> request_context(CoroutineId id, std::string_view key) const
        {
            return pool_.get_context<T>(id, key);
        }

        std::optional<CoroutineContextValue> request_context_any(CoroutineId id, std::string_view key) const
        {
            return pool_.get_context_any(id, key);
        }

        std::optional<CoroutineContextMap> request_context_map(CoroutineId id) const
        {
            return pool_.get_context_map(id);
        }

        template<typename T>
        bool set_request_context(CoroutineId id, std::string key, T &&value)
        {
            return pool_.set_context(id, std::move(key), std::forward<T>(value));
        }

        bool set_request_context_any(CoroutineId id, std::string key, CoroutineContextValue value)
        {
            return pool_.set_context_any(id, std::move(key), std::move(value));
        }

        bool complete(CoroutineId id, std::string payload = {})
        {
            return pool_.wake_completed(id, std::move(payload));
        }

        bool fail(CoroutineId id, std::string error)
        {
            return pool_.wake_failed(id, std::move(error));
        }

        bool cancel(CoroutineId id, std::string error = "canceled")
        {
            return pool_.wake_canceled(id, std::move(error));
        }

        std::size_t cancel_route(std::string_view route, std::string error = "route canceled")
        {
            return pool_.cancel_where(
                [route](CoroutineId, const CoroutineContextMap &context) {
                    const auto it = context.find("route");
                    if (it == context.end()) {
                        return false;
                    }
                    const std::string *route_value = std::any_cast<std::string>(&it->second);
                    return route_value && *route_value == route;
                },
                std::move(error));
        }

        std::size_t cancel_owner(std::string_view owner, std::string error = "owner canceled")
        {
            return pool_.cancel_where(
                [owner](CoroutineId, const CoroutineContextMap &context) {
                    const auto it = context.find("owner");
                    if (it == context.end()) {
                        return false;
                    }
                    const std::string *owner_value = std::any_cast<std::string>(&it->second);
                    return owner_value && *owner_value == owner;
                },
                std::move(error));
        }

        std::size_t cancel_all(std::string error = "service canceled")
        {
            return pool_.cancel_where(
                [](CoroutineId, const CoroutineContextMap &) {
                    return true;
                },
                std::move(error));
        }

        std::size_t cancel_where(PendingCoroutinePool::CancelPredicate predicate,
                                 std::string error = "canceled")
        {
            return pool_.cancel_where(std::move(predicate), std::move(error));
        }

        std::size_t tick(Clock::time_point now = Clock::now())
        {
            return pool_.wake_timeouts(now);
        }

        std::vector<SnapshotItem> snapshot_pending() const
        {
            std::vector<SnapshotItem> out;
            const auto snapshots = pool_.snapshot_entries();
            out.reserve(snapshots.size());
            for (const auto &entry : snapshots) {
                SnapshotItem item;
                item.id = entry.id;
                item.deadline_unix_ms = entry.deadline_unix_ms;
                item.context = entry.context;
                out.push_back(std::move(item));
            }
            return out;
        }

        bool restore_pending(const SnapshotItem &snapshot,
                             OnWake on_wake)
        {
            PendingCoroutineSnapshotEntry entry;
            entry.id = snapshot.id;
            entry.deadline_unix_ms = snapshot.deadline_unix_ms;
            entry.context = snapshot.context;
            return pool_.restore_from_snapshot(entry, std::move(on_wake));
        }

        std::size_t pending_size() const
        {
            return pool_.size();
        }

        bool contains(CoroutineId id) const
        {
            return pool_.contains(id);
        }

        PendingCoroutinePoolStats stats() const
        {
            return pool_.stats_snapshot();
        }

    private:
        PendingCoroutinePool pool_;
    };
}

#endif
