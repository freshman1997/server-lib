#include "event/event_loop.h"
#include "net/channel/channel.h"
#include "net/poller/poller.h"
#include "timer/wheel_timer_manager.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{
    int g_passed = 0;
    int g_failed = 0;

    void check(bool condition, const std::string &message)
    {
        if (condition) {
            ++g_passed;
        } else {
            std::cerr << "  FAIL: " << message << "\n";
            ++g_failed;
        }
    }

    class NoopPoller final : public yuan::net::Poller
    {
    public:
        bool init() override
        {
            return true;
        }

        uint64_t poll(uint32_t, std::vector<yuan::net::PollEvent> &) override
        {
            return 0;
        }

        void update_channel(yuan::net::Channel *channel) override
        {
            updated = channel;
            ++update_count;
        }

        void remove_channel(yuan::net::Channel *channel) override
        {
            removed = channel;
            ++remove_count;
        }

        yuan::net::Channel *updated = nullptr;
        yuan::net::Channel *removed = nullptr;
        int update_count = 0;
        int remove_count = 0;
    };

    void test_generation_validation()
    {
        std::cout << "  [EventToken] generation validation\n";

        NoopPoller poller;
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);
        yuan::net::Channel channel(42);
        channel.enable_read();

        loop.update_channel(&channel);
        const auto generation = channel.generation();

        yuan::net::PollEvent valid{42, yuan::net::Channel::READ_EVENT, generation};
        yuan::net::PollEvent zero_generation{42, yuan::net::Channel::READ_EVENT, 0};
        yuan::net::PollEvent stale{42, yuan::net::Channel::READ_EVENT, generation + 1};

        check(loop.accepts_poll_event_for_test(valid), "matching generation should be accepted");
        check(!loop.accepts_poll_event_for_test(zero_generation), "zero generation must not bypass validation");
        check(!loop.accepts_poll_event_for_test(stale), "stale generation should be rejected");

        loop.close_channel(&channel);
        check(channel.generation() != generation, "close_channel should invalidate old generation");
        check(!loop.accepts_poll_event_for_test(valid), "old event should be rejected after close_channel");
    }

    void test_reused_fd_stale_channel_is_ignored()
    {
        std::cout << "  [EventToken] reused fd ignores stale channel cleanup\n";

        NoopPoller poller;
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);

        yuan::net::Channel old_channel(77);
        old_channel.enable_read();
        loop.update_channel(&old_channel);
        const auto old_generation = old_channel.generation();

        yuan::net::Channel new_channel(77);
        new_channel.enable_read();
        loop.update_channel(&new_channel);
        const auto new_generation = new_channel.generation();

        check(new_generation != old_generation, "reused fd should get a new generation");

        yuan::net::PollEvent old_event{77, yuan::net::Channel::READ_EVENT, old_generation};
        yuan::net::PollEvent new_event{77, yuan::net::Channel::READ_EVENT, new_generation};
        check(!loop.accepts_poll_event_for_test(old_event), "old event should not match reused fd");
        check(loop.accepts_poll_event_for_test(new_event), "new event should match reused fd");

        const int remove_count_before_stale_close = poller.remove_count;
        loop.close_channel(&old_channel);
        check(poller.remove_count == remove_count_before_stale_close,
              "stale close_channel should not remove the active reused fd channel");
        check(loop.accepts_poll_event_for_test(new_event), "stale close should leave new channel registered");

        old_channel.disable_all();
        loop.update_channel(&old_channel);
        check(loop.accepts_poll_event_for_test(new_event), "stale empty update should leave new channel registered");

        loop.close_channel(&new_channel);
        check(!loop.accepts_poll_event_for_test(new_event), "active reused fd channel should close normally");
    }
}

int main()
{
    std::cout << "Running event token tests...\n\n";
    test_generation_validation();
    test_reused_fd_stale_channel_is_ignored();

    if (g_failed > 0) {
        std::cout << "FAILED: " << g_failed << " assertions failed\n";
        return 1;
    }

    std::cout << "All event token tests passed (" << g_passed << " assertions ok)\n";
    return 0;
}
