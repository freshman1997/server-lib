#include "game_coroutine/coroutine_pool.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
    using yuan::game_coroutine::CoroutineId;
    using yuan::game_coroutine::CoroutineWakeReason;
    using yuan::game_coroutine::CoroutineWakeResult;
    using yuan::game_coroutine::PendingCoroutinePool;

    struct RouteMeta
    {
        int shard = 0;
        std::string route;
    };

    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    int test_create_and_remote_resume_with_context()
    {
        PendingCoroutinePool pool;
        CoroutineWakeResult captured;
        bool called = false;

        const CoroutineId id = pool.create([&](const CoroutineWakeResult &result) {
            called = true;
            captured = result;
        });

        if (!require(id != 0, "created coroutine id should not be zero")) {
            return 10;
        }
        if (!require(pool.set_context(id, "trace_id", "abc-123"), "set_context should succeed")) {
            return 11;
        }
        if (!require(pool.set_context(id, "player_id", 42), "set_context second key should succeed")) {
            return 12;
        }
        RouteMeta meta;
        meta.shard = 7;
        meta.route = "inventory.commit";
        if (!require(pool.set_context(id, "route_meta", meta), "set_context object should succeed")) {
            return 22;
        }

        const auto before_wake_player = pool.get_context<int>(id, "player_id");
        if (!require(before_wake_player.has_value() && *before_wake_player == 42,
                     "typed get_context should read int before wake")) {
            return 23;
        }
        const auto before_wake_meta = pool.get_context<RouteMeta>(id, "route_meta");
        if (!require(before_wake_meta.has_value() && before_wake_meta->route == "inventory.commit",
                     "typed get_context should read object before wake")) {
            return 24;
        }
        if (!require(!pool.get_context<std::string>(id, "player_id").has_value(),
                     "typed get_context with wrong type should return empty")) {
            return 25;
        }

        if (!require(pool.wake_completed(id, "remote-ok"), "wake_completed should succeed")) {
            return 13;
        }
        if (!require(called, "callback should be called on wake")) {
            return 14;
        }
        if (!require(captured.id == id, "wake result should keep id")) {
            return 15;
        }
        if (!require(captured.reason == CoroutineWakeReason::completed, "wake reason should be completed")) {
            return 16;
        }
        if (!require(captured.payload == "remote-ok", "payload should round-trip")) {
            return 17;
        }
        const std::string *trace_id = captured.context_as<std::string>("trace_id");
        if (!require(trace_id && *trace_id == "abc-123", "context trace_id should round-trip")) {
            return 18;
        }
        const int *player_id = captured.context_as<int>("player_id");
        if (!require(player_id && *player_id == 42, "context player_id should round-trip")) {
            return 19;
        }
        const RouteMeta *route_meta = captured.context_as<RouteMeta>("route_meta");
        if (!require(route_meta && route_meta->shard == 7 && route_meta->route == "inventory.commit",
                     "context route_meta object should round-trip")) {
            return 21;
        }
        if (!require(!pool.contains(id) && pool.size() == 0, "woken coroutine should be removed from pool")) {
            return 20;
        }

        return 0;
    }

    int test_timeout_wake_with_error()
    {
        PendingCoroutinePool pool;
        CoroutineWakeResult captured;
        bool called = false;

        const auto timeout = std::chrono::milliseconds(5);
        const CoroutineId id = pool.create([&](const CoroutineWakeResult &result) {
            called = true;
            captured = result;
        }, timeout);

        if (!require(pool.contains(id), "pool should contain id before timeout")) {
            return 30;
        }

        const auto now = PendingCoroutinePool::Clock::now() + std::chrono::milliseconds(20);
        const std::size_t woken = pool.wake_timeouts(now);
        if (!require(woken == 1, "wake_timeouts should wake one entry")) {
            return 31;
        }
        if (!require(called, "timeout callback should be called")) {
            return 32;
        }
        if (!require(captured.id == id && captured.reason == CoroutineWakeReason::timed_out,
                     "timeout wake should return timed_out reason")) {
            return 33;
        }
        if (!require(captured.error == "timed out", "timeout wake should include default error")) {
            return 34;
        }
        if (!require(!pool.contains(id), "timeout wake should remove entry")) {
            return 35;
        }

        return 0;
    }

    int test_manual_id_and_failed_wake()
    {
        PendingCoroutinePool pool;
        CoroutineWakeResult captured;
        bool called = false;

        const CoroutineId manual_id = 998877;
        if (!require(pool.create_with_id(manual_id, [&](const CoroutineWakeResult &result) {
                called = true;
                captured = result;
            }), "create_with_id should succeed")) {
            return 40;
        }
        if (!require(!pool.create_with_id(manual_id, [](const CoroutineWakeResult &) {}),
                     "duplicate id should fail")) {
            return 41;
        }

        if (!require(pool.wake_failed(manual_id, "rpc failed"), "wake_failed should succeed")) {
            return 42;
        }
        if (!require(called, "failed wake callback should fire")) {
            return 43;
        }
        if (!require(captured.reason == CoroutineWakeReason::failed && captured.error == "rpc failed",
                     "failed wake should preserve error")) {
            return 44;
        }
        if (!require(!pool.wake_completed(manual_id, "again"), "waking removed id should fail")) {
            return 45;
        }

        return 0;
    }

    int test_cancel_by_tag_and_stats()
    {
        PendingCoroutinePool pool;
        int canceled_count = 0;
        int completed_count = 0;

        const CoroutineId id_match_1 = pool.create([&](const CoroutineWakeResult &result) {
            if (result.reason == CoroutineWakeReason::canceled) {
                ++canceled_count;
            }
        });
        const CoroutineId id_match_2 = pool.create([&](const CoroutineWakeResult &result) {
            if (result.reason == CoroutineWakeReason::canceled) {
                ++canceled_count;
            }
        });
        const CoroutineId id_keep = pool.create([&](const CoroutineWakeResult &result) {
            if (result.reason == CoroutineWakeReason::completed) {
                ++completed_count;
            }
        });

        if (!require(pool.set_context(id_match_1, "tag", "rpc.inventory"), "set tag for id_match_1 should succeed")) {
            return 50;
        }
        if (!require(pool.set_context(id_match_2, "tag", "rpc.inventory"), "set tag for id_match_2 should succeed")) {
            return 51;
        }
        if (!require(pool.set_context(id_keep, "tag", "rpc.profile"), "set tag for id_keep should succeed")) {
            return 52;
        }

        const std::size_t canceled = pool.cancel_by_tag("rpc.inventory", "router moved");
        if (!require(canceled == 2, "cancel_by_tag should cancel two entries")) {
            return 53;
        }
        if (!require(canceled_count == 2, "two callbacks should be canceled")) {
            return 54;
        }
        if (!require(pool.contains(id_keep), "non-matching tag should remain pending")) {
            return 55;
        }

        if (!require(pool.wake_completed(id_keep, "ok"), "remaining entry should complete")) {
            return 56;
        }
        if (!require(completed_count == 1, "one callback should complete")) {
            return 57;
        }

        (void)pool.wake_completed(id_keep, "again");

        const auto stats = pool.stats_snapshot();
        if (!require(stats.pending == 0, "stats pending should be zero")) {
            return 58;
        }
        if (!require(stats.created_total == 3, "stats created_total should be three")) {
            return 59;
        }
        if (!require(stats.wake_canceled_total == 2, "stats wake_canceled_total should be two")) {
            return 60;
        }
        if (!require(stats.wake_completed_total == 1, "stats wake_completed_total should be one")) {
            return 61;
        }
        if (!require(stats.wake_not_found_total >= 1, "stats wake_not_found_total should increase")) {
            return 62;
        }

        return 0;
    }
}

int main()
{
    if (const int result = test_create_and_remote_resume_with_context(); result != 0) {
        return result;
    }
    if (const int result = test_timeout_wake_with_error(); result != 0) {
        return result;
    }
    if (const int result = test_manual_id_and_failed_wake(); result != 0) {
        return result;
    }
    if (const int result = test_cancel_by_tag_and_stats(); result != 0) {
        return result;
    }

    std::cout << "coroutine pool tests passed\n";
    return EXIT_SUCCESS;
}
