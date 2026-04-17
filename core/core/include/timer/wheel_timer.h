#ifndef __WHEEL_TIMER_H__
#define __WHEEL_TIMER_H__
#include <cstdint>
#include <memory>
#include <vector>

#include "timer.h"

namespace yuan::timer
{

    class WheelTimerItem;
    class WheelTimer;
    class TimerTask;
    class WheelTimerManager;

    // 轮子
    class Wheel
    {
    public:
        Wheel(uint32_t size, uint32_t unit);
        virtual ~Wheel();

    public:
        std::size_t get_size() const;

        uint64_t time_unit() const;

        uint64_t unit_max_time() const;

        uint64_t get_remain_time() const;

        uint64_t get_passed_time() const;

        void place_timer(uint32_t idx, WheelTimer *timer);

        WheelTimerItem *tick(WheelTimerItem *newItem);

    private:
        uint32_t cursor_;
        uint64_t time_unit_;
        std::vector<WheelTimerItem *> items_;
    };

    // 轮子链接的item
    class WheelTimerItem
    {
    public:
        WheelTimerItem();
        virtual ~WheelTimerItem();

    public:
        void reset();
        void on_schedule(WheelTimer *timer);
        void on_delete(WheelTimer *timer);

        WheelTimer *begin() const;
        WheelTimer *end() const;
        WheelTimer *pop();

    private:
        WheelTimer *head_;
    };

    class WheelTimer : public timer::Timer
    {
        friend WheelTimerManager;

    public:
        WheelTimer(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period = 0);
        ~WheelTimer();

    public:
        virtual bool ready() const override;
        virtual void cancel();
        virtual void reset();
        virtual bool is_processing() const override;
        virtual bool is_done() const override;
        virtual bool is_cancel() const override;
        TimerTask *get_task() const override;

    public:
        WheelTimer *get_prev() const;
        WheelTimer *get_next() const;
        void set_prev(WheelTimer *timer);
        void set_next(WheelTimer *timer);
        void set_remain(uint64_t remain);
        uint64_t get_remain() const;
        void on_schedule(WheelTimerItem *item);
        void trigger();

    private:
        void bind_task(TimerTask *task);

    private:
        TimerState state_;
        int32_t period_;
        uint32_t period_counter_;
        uint32_t interval_;
        uint64_t remain_;
        TimerTask *task_;
        std::unique_ptr<TimerTask> owned_task_;
        WheelTimer *prev_;
        WheelTimer *next_;
        WheelTimerItem *item_;
    };
}

#endif
