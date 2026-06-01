#include "game_coroutine/pending_rpc_service.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

namespace
{
    using yuan::game_coroutine::CoroutineId;
    using yuan::game_coroutine::CoroutineWakeReason;
    using yuan::game_coroutine::CoroutineWakeResult;
    using yuan::game_coroutine::PendingRpcService;

    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    struct RpcMeta
    {
        int shard = 0;
        std::string peer;
    };

    int test_request_lifecycle_and_context()
    {
        PendingRpcService service;
        std::unordered_map<CoroutineId, CoroutineWakeResult> results;

        const CoroutineId id = service.begin_request(
            "inventory.commit",
            [&](const CoroutineWakeResult &result) {
                results[result.id] = result;
            },
            std::chrono::milliseconds(5000));

        if (!require(id != 0, "request id should not be zero")) {
            return 10;
        }

        RpcMeta meta;
        meta.shard = 3;
        meta.peer = "rpc-node-a";
        if (!require(service.set_request_context(id, "meta", meta), "set_request_context object should succeed")) {
            return 11;
        }
        if (!require(service.set_request_context(id, "player_id", 42), "set_request_context int should succeed")) {
            return 12;
        }

        if (!require(service.complete(id, "ok"), "complete should succeed")) {
            return 13;
        }

        const auto it = results.find(id);
        if (!require(it != results.end(), "callback should capture wake result")) {
            return 14;
        }
        if (!require(it->second.reason == CoroutineWakeReason::completed && it->second.payload == "ok",
                     "completed wake payload mismatch")) {
            return 15;
        }
        const RpcMeta *returned_meta = it->second.context_as<RpcMeta>("meta");
        if (!require(returned_meta && returned_meta->peer == "rpc-node-a",
                     "returned context object mismatch")) {
            return 16;
        }
        const int *player_id = it->second.context_as<int>("player_id");
        if (!require(player_id && *player_id == 42, "returned context int mismatch")) {
            return 17;
        }

        return 0;
    }

    int test_route_cancel_and_timeout()
    {
        PendingRpcService service;
        std::unordered_map<CoroutineId, CoroutineWakeResult> results;

        const auto base = PendingRpcService::Clock::now();
        const CoroutineId c1 = service.begin_request("mail.send", [&](const CoroutineWakeResult &r) { results[r.id] = r; }, std::chrono::milliseconds(5000));
        const CoroutineId c2 = service.begin_request("mail.send", [&](const CoroutineWakeResult &r) { results[r.id] = r; }, std::chrono::milliseconds(5000));
        const CoroutineId c3 = service.begin_request("profile.fetch", [&](const CoroutineWakeResult &r) { results[r.id] = r; }, std::chrono::milliseconds(30));

        const std::size_t canceled = service.cancel_route("mail.send", "route-drain");
        if (!require(canceled == 2, "cancel_route should cancel two requests")) {
            return 30;
        }

        if (!require(results[c1].reason == CoroutineWakeReason::canceled && results[c1].error == "route-drain",
                     "first route cancel mismatch")) {
            return 31;
        }
        if (!require(results[c2].reason == CoroutineWakeReason::canceled,
                     "second route cancel mismatch")) {
            return 32;
        }

        const std::size_t timed_out = service.tick(base + std::chrono::milliseconds(100));
        if (!require(timed_out == 1, "tick should timeout one request")) {
            return 33;
        }
        if (!require(results[c3].reason == CoroutineWakeReason::timed_out,
                     "timeout wake reason mismatch")) {
            return 34;
        }

        const auto stats = service.stats();
        if (!require(stats.pending == 0, "all requests should be resolved")) {
            return 35;
        }
        if (!require(stats.created_total == 3, "created_total should be three")) {
            return 36;
        }
        if (!require(stats.wake_canceled_total == 2, "wake_canceled_total should be two")) {
            return 37;
        }
        if (!require(stats.wake_timed_out_total == 1, "wake_timed_out_total should be one")) {
            return 38;
        }

        return 0;
    }

    int test_manual_id_and_predicate_cancel()
    {
        PendingRpcService service;
        std::unordered_map<CoroutineId, CoroutineWakeResult> results;

        const bool created_manual = service.begin_request_with_id(
            9001,
            "match.join",
            [&](const CoroutineWakeResult &r) {
                results[r.id] = r;
            },
            std::chrono::milliseconds(5000));
        if (!require(created_manual, "begin_request_with_id should succeed for fresh id")) {
            return 50;
        }
        if (!require(!service.begin_request_with_id(9001, "match.join", [](const CoroutineWakeResult &) {}),
                     "begin_request_with_id should reject duplicate id")) {
            return 51;
        }

        const CoroutineId c2 = service.begin_request("chat.send", [&](const CoroutineWakeResult &r) {
            results[r.id] = r;
        }, std::chrono::milliseconds(5000));
        (void)service.set_request_context(c2, "priority", 3);

        const std::size_t canceled = service.cancel_where(
            [](CoroutineId id, const yuan::game_coroutine::CoroutineContextMap &) {
                return id == 9001;
            },
            "manual-cancel");
        if (!require(canceled == 1, "cancel_where should cancel one matched request")) {
            return 52;
        }

        if (!require(results[9001].reason == CoroutineWakeReason::canceled && results[9001].error == "manual-cancel",
                     "manual id cancel result mismatch")) {
            return 53;
        }
        if (!require(service.contains(c2), "non-matching request should remain pending")) {
            return 54;
        }

        const auto priority = service.request_context<int>(c2, "priority");
        if (!require(priority.has_value() && *priority == 3,
                     "request_context should read pending typed context")) {
            return 55;
        }
        const auto route_any = service.request_context_any(c2, "route");
        if (!require(route_any.has_value() && std::any_cast<std::string>(&*route_any) != nullptr,
                     "request_context_any should read pending any context")) {
            return 56;
        }
        const auto map = service.request_context_map(c2);
        if (!require(map.has_value() && map->find("route") != map->end(),
                     "request_context_map should include route key")) {
            return 57;
        }

        if (!require(service.complete(c2, "ok"), "remaining request should complete")) {
            return 58;
        }
        if (!require(service.pending_size() == 0, "pending_size should be zero after completion")) {
            return 59;
        }

        return 0;
    }

    int test_cancel_owner_for_connection_close()
    {
        PendingRpcService service;
        std::unordered_map<CoroutineId, CoroutineWakeResult> results;

        const CoroutineId conn_a_1 = service.begin_request_for_owner(
            "conn#A",
            "inventory.read",
            [&](const CoroutineWakeResult &r) {
                results[r.id] = r;
            },
            std::chrono::milliseconds(5000));
        const CoroutineId conn_a_2 = service.begin_request_for_owner(
            "conn#A",
            "inventory.write",
            [&](const CoroutineWakeResult &r) {
                results[r.id] = r;
            },
            std::chrono::milliseconds(5000));
        const CoroutineId conn_b = service.begin_request_for_owner(
            "conn#B",
            "profile.read",
            [&](const CoroutineWakeResult &r) {
                results[r.id] = r;
            },
            std::chrono::milliseconds(5000));

        const std::size_t canceled = service.cancel_owner("conn#A", "connection closed");
        if (!require(canceled == 2, "cancel_owner should cancel all pending requests for owner")) {
            return 70;
        }
        if (!require(results[conn_a_1].reason == CoroutineWakeReason::canceled &&
                     results[conn_a_1].error == "connection closed",
                     "owner canceled request 1 mismatch")) {
            return 71;
        }
        if (!require(results[conn_a_2].reason == CoroutineWakeReason::canceled,
                     "owner canceled request 2 mismatch")) {
            return 72;
        }
        if (!require(service.contains(conn_b), "other owner request should remain pending")) {
            return 73;
        }

        if (!require(service.complete(conn_b, "ok"), "remaining owner should complete")) {
            return 74;
        }
        if (!require(results[conn_b].reason == CoroutineWakeReason::completed,
                     "remaining owner completion mismatch")) {
            return 75;
        }

        const auto stats = service.stats();
        if (!require(stats.wake_canceled_total == 2, "owner cancel should increase canceled stats")) {
            return 76;
        }

        return 0;
    }

    int test_snapshot_and_restore()
    {
        PendingRpcService producer;
        std::unordered_map<CoroutineId, CoroutineWakeResult> producer_results;

        const CoroutineId id = producer.begin_request_for_owner(
            "conn#restore",
            "profile.sync",
            [&](const CoroutineWakeResult &r) {
                producer_results[r.id] = r;
            },
            std::chrono::milliseconds(5000));
        (void)producer.set_request_context(id, "trace_id", std::string("trace-restore"));
        (void)producer.set_request_context(id, "shard", 9);

        const auto snapshots = producer.snapshot_pending();
        if (!require(snapshots.size() == 1, "snapshot should contain one pending entry")) {
            return 90;
        }
        if (!require(snapshots[0].id == id, "snapshot id should match original request")) {
            return 91;
        }

        PendingRpcService restored;
        std::unordered_map<CoroutineId, CoroutineWakeResult> restored_results;
        if (!require(restored.restore_pending(snapshots[0], [&](const CoroutineWakeResult &r) {
                restored_results[r.id] = r;
            }), "restore_pending should succeed")) {
            return 92;
        }
        if (!require(restored.contains(id), "restored service should contain restored request")) {
            return 93;
        }

        const auto restored_route = restored.request_context<std::string>(id, "route");
        if (!require(restored_route.has_value() && *restored_route == "profile.sync",
                     "restored route context should match")) {
            return 94;
        }
        const auto restored_owner = restored.request_context<std::string>(id, "owner");
        if (!require(restored_owner.has_value() && *restored_owner == "conn#restore",
                     "restored owner context should match")) {
            return 95;
        }
        const auto restored_shard = restored.request_context<int>(id, "shard");
        if (!require(restored_shard.has_value() && *restored_shard == 9,
                     "restored typed context should match")) {
            return 96;
        }

        if (!require(restored.complete(id, "restored-ok"), "restored request should complete")) {
            return 97;
        }
        if (!require(restored_results[id].reason == CoroutineWakeReason::completed &&
                     restored_results[id].payload == "restored-ok",
                     "restored wake result mismatch")) {
            return 98;
        }

        return 0;
    }
}

int main()
{
    if (const int result = test_request_lifecycle_and_context(); result != 0) {
        return result;
    }
    if (const int result = test_route_cancel_and_timeout(); result != 0) {
        return result;
    }
    if (const int result = test_manual_id_and_predicate_cancel(); result != 0) {
        return result;
    }
    if (const int result = test_cancel_owner_for_connection_close(); result != 0) {
        return result;
    }
    if (const int result = test_snapshot_and_restore(); result != 0) {
        return result;
    }

    std::cout << "pending rpc service tests passed\n";
    return EXIT_SUCCESS;
}
