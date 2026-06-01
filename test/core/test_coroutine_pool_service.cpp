#include "game_coroutine/coroutine_pool.h"

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
    using yuan::game_coroutine::PendingCoroutinePool;
    using yuan::game_coroutine::PendingCoroutinePoolStats;

    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    class TestCoroutineService
    {
    public:
        CoroutineId dispatch(std::string tag, int timeout_ms)
        {
            CoroutineId id = 0;
            const auto timeout = std::chrono::milliseconds(timeout_ms);
            id = pool_.create([this](const CoroutineWakeResult &result) {
                results_[result.id] = result;
            }, timeout);

            (void)pool_.set_context(id, "tag", std::move(tag));
            (void)pool_.set_context(id, "trace_id", std::string("trace-") + std::to_string(id));
            return id;
        }

        bool on_remote_reply(CoroutineId id, std::string payload)
        {
            return pool_.wake_completed(id, std::move(payload));
        }

        bool on_remote_error(CoroutineId id, std::string error)
        {
            return pool_.wake_failed(id, std::move(error));
        }

        std::size_t cancel_tag(std::string_view tag, std::string error)
        {
            return pool_.cancel_by_tag(tag, std::move(error));
        }

        std::size_t tick_timeouts(PendingCoroutinePool::Clock::time_point now)
        {
            return pool_.wake_timeouts(now);
        }

        std::optional<CoroutineWakeResult> result_of(CoroutineId id) const
        {
            const auto it = results_.find(id);
            if (it == results_.end()) {
                return std::nullopt;
            }
            return it->second;
        }

        PendingCoroutinePoolStats stats() const
        {
            return pool_.stats_snapshot();
        }

    private:
        PendingCoroutinePool pool_;
        std::unordered_map<CoroutineId, CoroutineWakeResult> results_;
    };

    int test_service_flow()
    {
        TestCoroutineService service;

        const auto base = PendingCoroutinePool::Clock::now();
        const CoroutineId ok_id = service.dispatch("rpc.inventory", 5000);
        const CoroutineId fail_id = service.dispatch("rpc.profile", 5000);
        const CoroutineId timeout_id = service.dispatch("rpc.mail", 20);

        if (!require(service.on_remote_reply(ok_id, "ok-payload"), "remote reply should wake completed")) {
            return 10;
        }
        if (!require(service.on_remote_error(fail_id, "remote-failed"), "remote error should wake failed")) {
            return 11;
        }

        const std::size_t timeout_woken = service.tick_timeouts(base + std::chrono::milliseconds(200));
        if (!require(timeout_woken == 1, "timeout tick should wake exactly one coroutine")) {
            return 12;
        }

        const auto ok = service.result_of(ok_id);
        const auto fail = service.result_of(fail_id);
        const auto timeout = service.result_of(timeout_id);
        if (!require(ok.has_value() && ok->reason == CoroutineWakeReason::completed && ok->payload == "ok-payload",
                     "completed coroutine result mismatch")) {
            return 13;
        }
        if (!require(fail.has_value() && fail->reason == CoroutineWakeReason::failed && fail->error == "remote-failed",
                     "failed coroutine result mismatch")) {
            return 14;
        }
        if (!require(timeout.has_value() && timeout->reason == CoroutineWakeReason::timed_out,
                     "timed out coroutine result mismatch")) {
            return 15;
        }

        const std::string *trace_id = ok->context_as<std::string>("trace_id");
        if (!require(trace_id && !trace_id->empty(), "context should survive wake callback")) {
            return 16;
        }

        return 0;
    }

    int test_service_batch_cancel_and_stats()
    {
        TestCoroutineService service;
        const CoroutineId c1 = service.dispatch("rpc.match", 5000);
        const CoroutineId c2 = service.dispatch("rpc.match", 5000);
        const CoroutineId keep = service.dispatch("rpc.chat", 5000);

        const std::size_t canceled = service.cancel_tag("rpc.match", "service-shutdown");
        if (!require(canceled == 2, "cancel_tag should cancel matched requests")) {
            return 30;
        }

        const auto r1 = service.result_of(c1);
        const auto r2 = service.result_of(c2);
        if (!require(r1.has_value() && r1->reason == CoroutineWakeReason::canceled && r1->error == "service-shutdown",
                     "first canceled result mismatch")) {
            return 31;
        }
        if (!require(r2.has_value() && r2->reason == CoroutineWakeReason::canceled,
                     "second canceled result mismatch")) {
            return 32;
        }

        if (!require(service.on_remote_reply(keep, "kept"), "kept request should still be active")) {
            return 33;
        }

        const auto stats = service.stats();
        if (!require(stats.pending == 0, "stats pending should be zero after wakes")) {
            return 34;
        }
        if (!require(stats.created_total == 3, "stats created_total should be three")) {
            return 35;
        }
        if (!require(stats.wake_canceled_total == 2, "stats wake_canceled_total should be two")) {
            return 36;
        }
        if (!require(stats.wake_completed_total == 1, "stats wake_completed_total should be one")) {
            return 37;
        }

        return 0;
    }
}

int main()
{
    if (const int result = test_service_flow(); result != 0) {
        return result;
    }
    if (const int result = test_service_batch_cancel_and_stats(); result != 0) {
        return result;
    }

    std::cout << "coroutine pool service tests passed\n";
    return EXIT_SUCCESS;
}
