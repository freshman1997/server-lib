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
        }

        void remove_channel(yuan::net::Channel *channel) override
        {
            removed = channel;
        }

        yuan::net::Channel *updated = nullptr;
        yuan::net::Channel *removed = nullptr;
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
}

int main()
{
    std::cout << "Running event token tests...\n\n";
    test_generation_validation();

    if (g_failed > 0) {
        std::cout << "FAILED: " << g_failed << " assertions failed\n";
        return 1;
    }

    std::cout << "All event token tests passed (" << g_passed << " assertions ok)\n";
    return 0;
}
