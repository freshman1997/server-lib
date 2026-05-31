#include "game_coroutine/switch_coroutine.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
    using yuan::game_coroutine::SwitchCoroutineError;
    using yuan::game_coroutine::SwitchCoroutineResult;
    using yuan::game_coroutine::SwitchCoroutineStatus;

    struct InventoryService
    {
        int apples = 10;
        int reserved = 0;
        int ticks_until_ready = 0;
        bool operation_active = false;
        bool operation_done = false;
        bool fail_next_commit = false;

        void start_load()
        {
            start_operation(2);
        }

        void start_reserve(int amount)
        {
            reserved = amount;
            start_operation(1);
        }

        void start_commit()
        {
            start_operation(2);
        }

        void tick()
        {
            if (!operation_active) {
                return;
            }
            if (ticks_until_ready > 0) {
                --ticks_until_ready;
            }
            if (ticks_until_ready == 0) {
                operation_active = false;
                operation_done = true;
            }
        }

        bool ready() const noexcept
        {
            return operation_done;
        }

        void consume_ready() noexcept
        {
            operation_done = false;
        }

        bool commit_reserved()
        {
            if (fail_next_commit) {
                fail_next_commit = false;
                reserved = 0;
                return false;
            }
            apples -= reserved;
            reserved = 0;
            return true;
        }

    private:
        void start_operation(int ticks)
        {
            operation_active = true;
            operation_done = false;
            ticks_until_ready = ticks;
        }
    };

    struct DeductItemsFlow
    {
        SwitchCoroutineResult<int> ctx;
        InventoryService *inventory = nullptr;
        int amount = 0;
        bool load_started = false;
        bool reserve_started = false;
        bool commit_started = false;
        bool canceled = false;
        int trace_events = 0;

        SwitchCoroutineStatus resume()
        {
            YUAN_SWITCH_COROUTINE_BEGIN(ctx);

            if (!inventory) {
                YUAN_SWITCH_COROUTINE_FAIL_CODE(ctx, SwitchCoroutineError::invalid_state, "inventory service is missing");
            }
            if (canceled) {
                YUAN_SWITCH_COROUTINE_CANCEL(ctx, "deduct canceled");
            }

            load_started = true;
            inventory->start_load();
            YUAN_SWITCH_COROUTINE_WAIT_UNTIL(ctx, 10, inventory->ready());
            inventory->consume_ready();

            if (inventory->apples < amount) {
                YUAN_SWITCH_COROUTINE_FAIL_CODE(ctx, SwitchCoroutineError::insufficient_resource, "not enough items");
            }

            reserve_started = true;
            inventory->start_reserve(amount);
            YUAN_SWITCH_COROUTINE_COMPENSATE(ctx, [inv = inventory]() {
                inv->reserved = 0;
            });
            YUAN_SWITCH_COROUTINE_WAIT_UNTIL(ctx, 20, inventory->ready());
            inventory->consume_ready();

            commit_started = true;
            inventory->start_commit();
            YUAN_SWITCH_COROUTINE_WAIT_UNTIL(ctx, 30, inventory->ready());
            inventory->consume_ready();

            if (!inventory->commit_reserved()) {
                YUAN_SWITCH_COROUTINE_FAIL(ctx, "commit failed");
            }

            YUAN_SWITCH_COROUTINE_RETURN(ctx, inventory->apples);
            YUAN_SWITCH_COROUTINE_END(ctx);
        }

        void reset(InventoryService &service, int deduct_amount)
        {
            ctx.reset();
            ctx.set_name("deduct-items");
            ctx.set_trace_hook([this](const auto &) {
                ++trace_events;
            });
            inventory = &service;
            amount = deduct_amount;
            load_started = false;
            reserve_started = false;
            commit_started = false;
            canceled = false;
            trace_events = 0;
        }
    };

    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    int test_successful_deduct()
    {
        InventoryService inventory;
        DeductItemsFlow flow;
        flow.reset(inventory, 3);

        if (!require(flow.resume() == SwitchCoroutineStatus::pending, "load should suspend")) {
            return 10;
        }
        if (!require(flow.load_started && !flow.reserve_started, "only load should have started")) {
            return 11;
        }

        inventory.tick();
        if (!require(flow.resume() == SwitchCoroutineStatus::pending, "load should still be pending")) {
            return 12;
        }
        inventory.tick();
        if (!require(flow.resume() == SwitchCoroutineStatus::pending, "reserve should suspend")) {
            return 13;
        }
        if (!require(flow.reserve_started && inventory.reserved == 3, "reserve should have started")) {
            return 14;
        }

        inventory.tick();
        if (!require(flow.resume() == SwitchCoroutineStatus::pending, "commit should suspend")) {
            return 15;
        }
        if (!require(flow.commit_started, "commit should have started")) {
            return 16;
        }

        inventory.tick();
        if (!require(flow.resume() == SwitchCoroutineStatus::pending, "commit should still be pending")) {
            return 17;
        }
        inventory.tick();
        if (!require(flow.resume() == SwitchCoroutineStatus::completed, "deduct should complete")) {
            return 18;
        }
        if (!require(flow.ctx.has_value() && flow.ctx.value() == 7 && inventory.apples == 7,
                     "deduct should return remaining item count")) {
            return 19;
        }
        if (!require(flow.trace_events > 0, "trace hook should observe coroutine events")) {
            return 21;
        }
        if (!require(flow.resume() == SwitchCoroutineStatus::completed,
                     "completed flow should remain completed")) {
            return 20;
        }

        return 0;
    }

    int test_not_enough_items()
    {
        InventoryService inventory;
        DeductItemsFlow flow;
        flow.reset(inventory, 99);

        (void)flow.resume();
        inventory.tick();
        (void)flow.resume();
        inventory.tick();
        if (!require(flow.resume() == SwitchCoroutineStatus::failed, "flow should fail after load")) {
            return 30;
        }
        if (!require(flow.ctx.error() == "not enough items", "flow should report not enough items")) {
            return 31;
        }
        if (!require(flow.ctx.error_code() == SwitchCoroutineError::insufficient_resource,
                     "flow should report typed insufficient resource error")) {
            return 33;
        }
        if (!require(inventory.apples == 10 && inventory.reserved == 0, "failed flow should not mutate stock")) {
            return 32;
        }

        return 0;
    }

    int test_commit_failure_and_reset()
    {
        InventoryService inventory;
        DeductItemsFlow flow;
        flow.reset(inventory, 4);
        inventory.fail_next_commit = true;

        while (flow.resume() == SwitchCoroutineStatus::pending) {
            inventory.tick();
        }
        if (!require(flow.ctx.failed(), "commit failure should fail flow")) {
            return 40;
        }
        if (!require(flow.ctx.error() == "commit failed", "commit failure should be reported")) {
            return 41;
        }
        if (!require(inventory.apples == 10 && inventory.reserved == 0, "failed commit should release reservation")) {
            return 42;
        }

        flow.reset(inventory, 2);
        while (flow.resume() == SwitchCoroutineStatus::pending) {
            inventory.tick();
        }
        if (!require(flow.ctx.completed() && flow.ctx.value() == 8, "reset flow should be reusable")) {
            return 43;
        }

        return 0;
    }

    int test_cancel_runs_compensation()
    {
        InventoryService inventory;
        inventory.reserved = 5;
        DeductItemsFlow flow;
        flow.reset(inventory, 1);
        flow.ctx.add_compensation([&]() {
            inventory.reserved = 0;
        });
        flow.canceled = true;

        if (!require(flow.resume() == SwitchCoroutineStatus::failed, "canceled flow should fail")) {
            return 50;
        }
        if (!require(flow.ctx.canceled() && flow.ctx.error() == "deduct canceled",
                     "canceled flow should expose cancel status")) {
            return 51;
        }
        if (!require(inventory.reserved == 0, "cancel should run compensation")) {
            return 52;
        }

        return 0;
    }
}

int main()
{
    if (const int result = test_successful_deduct(); result != 0) {
        return result;
    }
    if (const int result = test_not_enough_items(); result != 0) {
        return result;
    }
    if (const int result = test_commit_failure_and_reset(); result != 0) {
        return result;
    }
    if (const int result = test_cancel_runs_compensation(); result != 0) {
        return result;
    }

    std::cout << "switch coroutine tests passed\n";
    return EXIT_SUCCESS;
}
