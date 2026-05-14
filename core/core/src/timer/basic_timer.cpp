#include "timer/basic_timer.h"

#include "timer/timer_task.h"

namespace yuan::timer
{
    BasicTimer::BasicTimer(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period)
        : state_(TimerState::init),
          period_(period),
          period_counter_(0),
          interval_(interval),
          deadline_(timeout),
          task_(nullptr),
          handle_state_(std::make_shared<TimerHandleState>())
    {
        bind_task(task);
        handle_state_->bind([this]() {
            cancel();
        });
    }

    BasicTimer::~BasicTimer()
    {
        if (handle_state_) {
            handle_state_->clear();
        }

        if (task_) {
            task_->on_finished(this);
            task_ = nullptr;
        }
    }

    bool BasicTimer::ready() const
    {
        return state_ == TimerState::init;
    }

    void BasicTimer::cancel()
    {
        state_ = TimerState::cancal;
        if (handle_state_) {
            handle_state_->clear();
        }
        if (!owned_task_) {
            task_ = nullptr;
        }
    }

    void BasicTimer::reset()
    {
        state_ = TimerState::init;
        period_counter_ = 0;
    }

    bool BasicTimer::is_processing() const
    {
        return state_ == TimerState::processing;
    }

    bool BasicTimer::is_done() const
    {
        return state_ == TimerState::done;
    }

    bool BasicTimer::is_cancel() const
    {
        return state_ == TimerState::cancal;
    }

    TimerTask *BasicTimer::get_task() const
    {
        return task_;
    }

    std::shared_ptr<TimerHandleState> BasicTimer::handle_state() const
    {
        return handle_state_;
    }

    uint64_t BasicTimer::deadline() const noexcept
    {
        return deadline_;
    }

    void BasicTimer::set_deadline(uint64_t deadline) noexcept
    {
        deadline_ = deadline;
    }

    uint32_t BasicTimer::interval() const noexcept
    {
        return interval_;
    }

    uint64_t BasicTimer::next_deadline(uint64_t now) const noexcept
    {
        const uint64_t interval = interval_ == 0 ? 1ULL : interval_;
        uint64_t next = deadline_ + interval;
        if (next <= now) {
            next = now + interval;
        }
        return next;
    }

    void BasicTimer::trigger()
    {
        if (state_ != TimerState::init) {
            return;
        }

        state_ = TimerState::processing;
        if (task_) {
            task_->on_timer(this);
        }

        if (state_ == TimerState::cancal) {
            return;
        }

        state_ = TimerState::done;
        if (period_ != 0 && interval_ != 0 && task_) {
            if (period_ > 0) {
                ++period_counter_;
                if (period_counter_ == static_cast<uint32_t>(period_)) {
                    if (handle_state_) {
                        handle_state_->clear();
                    }
                    return;
                }
                state_ = TimerState::init;
            } else {
                reset();
            }
        }

        if (state_ == TimerState::done && handle_state_) {
            handle_state_->clear();
        }
    }

    void BasicTimer::bind_task(TimerTask *task)
    {
        if (!task) {
            owned_task_.reset();
            task_ = nullptr;
            return;
        }

        if (task->need_free()) {
            owned_task_.reset(task);
            task_ = owned_task_ ? &*owned_task_ : nullptr;
            return;
        }

        owned_task_.reset();
        task_ = task;
    }
}
