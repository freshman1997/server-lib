#include "timer/wheel_timer.h"
#include "timer/timer_task.h"

#include <cstddef>
#include <cstdint>

namespace timer 
{
    Wheel::Wheel(uint32_t size, uint32_t unit)
    {
        items_.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
            items_[i] = new WheelTimerItem;
        }

        cursor_ = 0;
        time_unit_ = unit;
    }

    Wheel::~Wheel()
    {
        uint32_t size = items_.size();
        for (auto &item : items_) {
            delete item;
        }

        items_.clear();
    }

    std::size_t Wheel::get_size() const
    {
        return items_.size();
    }

    uint64_t Wheel::time_unit() const
    {
        return time_unit_;
    }

    uint64_t Wheel::unit_max_time() const
    {
        return time_unit_ * items_.size();
    }

    uint64_t Wheel::get_remain_time() const
    {
        return (items_.size() - cursor_) * time_unit_;
    }

    uint64_t Wheel::get_passed_time() const
    {
        return cursor_ * time_unit_;
    }

    void Wheel::place_timer(uint32_t idx, WheelTimer *timer)
    {
        if (idx % items_.size() == 0) {
            // 放置时候不能覆盖当前的，所以至少往下一个
            idx = 1;
        }

        uint32_t pos = (cursor_ + idx) % items_.size();
        items_[pos]->on_schedule(timer);
    }

    WheelTimerItem * Wheel::tick(WheelTimerItem *newItem)
    {
        cursor_ = (cursor_ + 1) % items_.size();
        WheelTimerItem *item = items_[cursor_];
        items_[cursor_] = newItem;
        return item;
    }

    
    WheelTimerItem::WheelTimerItem() : head_(nullptr)
    {}

    WheelTimerItem::~WheelTimerItem()
    {
        for (WheelTimer *timer = head_; timer != nullptr; timer = timer->get_next()) {
            timer->on_schedule(nullptr);
        }
    }

    void WheelTimerItem::reset()
    {
        head_ = nullptr;
    }

    void WheelTimerItem::on_schedule(WheelTimer *timer)
    {
        timer->on_schedule(this);
        if (!head_) {
            timer->set_prev(nullptr);
            timer->set_next(nullptr);
        } else {
            timer->set_prev(nullptr);
            timer->set_next(head_);
            head_->set_prev(timer);
        }

        head_ = timer;
    }

    void WheelTimerItem::on_delete(WheelTimer *timer)
    {
        if (head_ == timer) {
            head_ = timer->get_next();
            if (head_) {
                head_->set_prev(nullptr);
            }
        } else {
            if (timer->get_prev()) {
                timer->get_prev()->set_next(timer->get_next());
            }

            if (timer->get_next()) {
                timer->get_next()->set_prev(timer->get_prev());
            }
        }
    }

    WheelTimer * WheelTimerItem::begin()
    {
        return head_;
    }

    WheelTimer * WheelTimerItem::end()
    {
        return nullptr;
    }

    WheelTimer * WheelTimerItem::pop()
    {
        WheelTimer *timer = head_;
        if (head_) {
            head_ = head_->get_next();
            if (head_) {
                head_->set_prev(nullptr);
            }

            timer->set_prev(nullptr);
            timer->set_next(nullptr);
        }

        return timer;
    }


    WheelTimer::WheelTimer(uint32_t timeout, uint32_t interval, TimerTask * task, int32_t period) 
    {
        period_ = period;
        period_counter_ = 0;
        state_ = TimerState::init;
        interval_ = interval;
        remain_ = timeout;
        task_ = task;
        prev_ = nullptr;
        next_ = nullptr;
        item_ = nullptr;
    }

    WheelTimer::~WheelTimer()
    {
        if (task_) {
            task_->on_finished(this);
            task_ = nullptr;
        }

        if (item_) {
            item_->on_delete(this);
            item_ = nullptr;
        }
    }

    bool WheelTimer::ready()
    {
        return state_ == TimerState::init;
    }

    void WheelTimer::cancel()
    {
        state_ = TimerState::cancal;
        if (task_ && task_->need_free()) {
            return;
        }
        task_ = nullptr;
    }

    void WheelTimer::reset()
    {
        state_ = TimerState::init;
        period_counter_ = 0;
    }

    bool WheelTimer::is_processing()
    {
        return state_ == TimerState::processing;
    }

    bool WheelTimer::is_done()
    {
        return state_ == TimerState::done;
    }

    bool WheelTimer::is_cancel()
    {
        return state_ == TimerState::cancal;
    }

    WheelTimer * WheelTimer::get_prev()
    {
        return prev_;
    }

    WheelTimer * WheelTimer::get_next()
    {
        return next_;
    }

    void WheelTimer::set_prev(WheelTimer *timer)
    {
        prev_ = timer;
    }

    void WheelTimer::set_next(WheelTimer *timer)
    {
        next_ = timer;
    }

    void WheelTimer::set_remain(uint64_t remain)
    {
        remain_ = remain;
    }

    uint64_t WheelTimer::get_remain() const
    {
        return remain_;
    }

    void WheelTimer::on_schedule(WheelTimerItem *item)
    {
        item_ = item;
    }

    void WheelTimer::trigger()
    {
        if (state_ != TimerState::init) {
            return;
        }

        state_ = TimerState::processing;
        if (task_) {
            task_->on_timer(this);
        }

        state_ = TimerState::done;
        if (period_ != 0) {
            if (interval_ != 0 && task_) {
                remain_ = interval_;
                if (period_ > 0) {
                    ++period_counter_;
                    if (period_counter_ == period_) {
                        return;
                    }
                    state_ = TimerState::init;
                } else {
                    reset();
                }
            }
        }
    }

    TimerTask * WheelTimer::get_task()
    {
        return task_;
    }
}