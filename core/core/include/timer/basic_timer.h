#ifndef __YUAN_TIMER_BASIC_TIMER_H__
#define __YUAN_TIMER_BASIC_TIMER_H__

#include "timer.h"

#include <cstdint>
#include <memory>

namespace yuan::timer
{
    class TimerTask;

    class BasicTimer : public Timer
    {
    public:
        BasicTimer(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period = 0);
        ~BasicTimer() override;

        bool ready() const override;
        void cancel() override;
        void reset() override;
        bool is_processing() const override;
        bool is_done() const override;
        bool is_cancel() const override;
        TimerTask *get_task() const override;
        std::shared_ptr<TimerHandleState> handle_state() const override;

        uint64_t deadline() const noexcept;
        void set_deadline(uint64_t deadline) noexcept;
        uint32_t interval() const noexcept;
        uint64_t next_deadline(uint64_t now) const noexcept;
        void trigger();

    private:
        void bind_task(TimerTask *task);

    private:
        TimerState state_;
        int32_t period_;
        uint32_t period_counter_;
        uint32_t interval_;
        uint64_t deadline_;
        TimerTask *task_;
        std::unique_ptr<TimerTask> owned_task_;
        std::shared_ptr<TimerHandleState> handle_state_;
    };
}

#endif
