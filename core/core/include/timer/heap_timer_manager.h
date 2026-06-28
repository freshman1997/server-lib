#ifndef __YUAN_TIMER_HEAP_TIMER_MANAGER_H__
#define __YUAN_TIMER_HEAP_TIMER_MANAGER_H__

#include "timer_manager.h"

#include <cstdint>
#include <memory>
#include <queue>
#include <vector>

namespace yuan::timer
{
    class BasicTimer;

    class HeapTimerManager : public TimerManager
    {
    public:
        explicit HeapTimerManager(uint32_t time_unit = 1);
        ~HeapTimerManager() override;

        void tick() override;
        uint32_t get_time_unit() const override;
        const char *backend_name() const override;
        uint32_t get_poll_timeout_ms(uint32_t idle_timeout_ms, uint32_t active_timeout_cap_ms) override;

    protected:
        Timer *timeout(uint32_t milliseconds, TimerTask *task) override;
        Timer *interval(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period = 0) override;
        bool schedule(Timer *timer) override;

    private:
        struct Entry
        {
            uint64_t deadline = 0;
            uint64_t sequence = 0;
            BasicTimer *timer = nullptr;
        };

        struct LaterDeadline
        {
            bool operator()(const Entry &left, const Entry &right) const noexcept;
        };

        Timer *schedule(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period);
        void push(BasicTimer *timer);
        void cleanup_cancelled(bool force = false);

    private:
        uint32_t time_unit_;
        uint64_t sequence_;
        std::size_t stale_timer_count_ = 0;
        std::priority_queue<Entry, std::vector<Entry>, LaterDeadline> heap_;
        std::vector<std::unique_ptr<BasicTimer> > timers_;
    };
}

#endif
