#include "base/time.h"
#include "event/event_loop.h"
#include "net/channel/channel.h"
#include "net/poller/poller.h"
#include "timer/heap_timer_manager.h"
#include "timer/timer_manager.h"
#include "timer/timer_manager_factory.h"
#include "timer/wheel_timer_manager.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    class RecordingPoller : public yuan::net::Poller
    {
    public:
        explicit RecordingPoller(yuan::net::EventLoop *loop = nullptr)
            : loop_(loop)
        {
        }

        void set_loop(yuan::net::EventLoop *loop)
        {
            loop_ = loop;
        }

        bool init() override
        {
            return true;
        }

        uint64_t poll(uint32_t timeout, std::vector<yuan::net::PollEvent> &events) override
        {
            events.clear();
            last_timeout = timeout;
            ++poll_count;
            if (loop_) {
                loop_->quit();
            }
            return 0;
        }

        void update_channel(yuan::net::Channel *channel) override
        {
            (void)channel;
        }

        void remove_channel(yuan::net::Channel *channel) override
        {
            (void)channel;
        }

        uint32_t last_timeout = 0;
        uint32_t poll_count = 0;

    private:
        yuan::net::EventLoop *loop_;
    };

    void run_once(yuan::timer::TimerManager &manager)
    {
        int fired = 0;
        auto timer = manager.after(10, [&fired]() {
            ++fired;
        });

        assert(timer);
        yuan::base::time::advance_steady_time_for_test(10);
        manager.run_due_timers();
        assert(fired == 1);
        assert(!timer);
    }

    void run_periodic(yuan::timer::TimerManager &manager)
    {
        int fired = 0;
        auto timer = manager.every(5, 5, [&fired]() {
            ++fired;
        }, 3);

        assert(timer);
        for (int i = 0; i < 3; ++i) {
            yuan::base::time::advance_steady_time_for_test(5);
            manager.run_due_timers();
        }
        assert(fired == 3);
        assert(!timer);
    }

    void run_cancelled_timer_cleanup(yuan::timer::TimerManager &manager)
    {
        int fired = 0;
        auto soon = manager.after(10, [&fired]() {
            ++fired;
        });
        auto later = manager.after(1000, [&fired]() {
            ++fired;
        });

        assert(soon);
        assert(later);
        later.cancel();
        assert(!later);
        assert(manager.poll_timeout(50, 50) == 10);

        yuan::base::time::advance_steady_time_for_test(10);
        manager.run_due_timers();
        assert(fired == 1);
        assert(!soon);
        assert(manager.poll_timeout(50, 50) == 50);
    }

    void registered_channel_uses_capped_timer_timeout()
    {
        yuan::timer::HeapTimerManager timers;
        auto long_timer = timers.after(30000, []() {});
        assert(long_timer);

        RecordingPoller poller;
        yuan::net::EventLoop loop(&poller, &timers);
        poller.set_loop(&loop);

        yuan::net::Channel channel(12345);
        channel.enable_read();
        loop.update_channel(&channel);
        loop.loop();

        assert(poller.poll_count == 1);
        assert(poller.last_timeout == 50);
        long_timer.cancel();
    }
}

int main()
{
    yuan::base::time::set_steady_time_for_test(1000);
    {
        yuan::timer::HeapTimerManager manager;
        run_once(manager);
        run_periodic(manager);
        run_cancelled_timer_cleanup(manager);
    }

    yuan::base::time::set_steady_time_for_test(2000);
    {
        yuan::timer::WheelTimerManager manager;
        run_once(manager);
        run_periodic(manager);
    }

    auto heap = yuan::timer::create_timer_manager(yuan::timer::TimerBackend::heap);
    assert(heap);
    assert(std::string(heap->backend_name()) == "heap");

    auto automatic = yuan::timer::create_timer_manager(yuan::timer::TimerBackend::automatic);
    assert(automatic);
    assert(std::string(automatic->backend_name()) == "heap");
    yuan::base::time::set_steady_time_for_test(2500);
    auto automatic_long_timer = automatic->after(30000, []() {});
    assert(automatic_long_timer);
    assert(automatic->poll_timeout(50, 50) == 50);
    automatic_long_timer.cancel();

    auto wheel = yuan::timer::create_timer_manager(yuan::timer::TimerBackend::wheel);
    assert(wheel);
    assert(std::string(wheel->backend_name()) == "wheel");

    yuan::base::time::set_steady_time_for_test(3000);
    auto long_timer = wheel->after(30000, []() {});
    assert(long_timer);
    assert(wheel->poll_timeout(50, 50) == wheel->get_time_unit());
    assert(wheel->poll_timeout(50, 1) == 1);
    long_timer.cancel();

    yuan::base::time::set_steady_time_for_test(4000);
    registered_channel_uses_capped_timer_timeout();

    yuan::base::time::reset_test_time();
    std::cout << "timer backend test passed\n";
    return 0;
}
