#include <cstdint>
#include <algorithm>

#include "base/time.h"
#include "timer/wheel_timer_manager.h"
#include "timer/wheel_timer.h"

namespace yuan::timer
{
    namespace
    {
        template <typename T>
        T *ptr_of(const std::unique_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }
    }

    WheelTimerManager::WheelTimerManager()
    {
        count_ = 1000;
        time_unit_ = 1;
        helper_item_ = std::make_unique<WheelTimerItem>();

        base::time::init_time(time_unit_);
    }

    WheelTimerManager::~WheelTimerManager()
    {
    }

    Timer *WheelTimerManager::timeout(uint32_t milliseconds, TimerTask * task)
    {
        return schedule(milliseconds, 0, task, 0);
    }

    Timer *WheelTimerManager::interval(uint32_t timeout, uint32_t interval, TimerTask * task, int32_t period)
    {
        return schedule(timeout, interval, task, period);
    }

    bool WheelTimerManager::schedule(Timer * timer)
    {
        if (!timer) {
            return false;
        }

        WheelTimer *wheel_timer = dynamic_cast<WheelTimer *>(timer);
        if (!wheel_timer) {
            return false;
        }

        place_timer(wheel_timer);

        return true;
    }

    Timer *WheelTimerManager::schedule(uint32_t timeout, uint32_t interval, TimerTask * task, int32_t period)
    {
        auto owned_timer = std::make_unique<WheelTimer>(timeout, interval, task, period);
        WheelTimer *timer = &*owned_timer;
        timers_.push_back(std::move(owned_timer));
        place_timer(timer);
        return timer;
    }

    void WheelTimerManager::place_timer(WheelTimer * timer)
    {
        uint64_t time_unit = time_unit_;
        for (auto it = wheels_.begin(); it != wheels_.end(); ++it) {
            uint64_t remain = timer->get_remain();
            time_unit = (*it)->unit_max_time();
            if (remain < time_unit) {
                (*it)->place_timer(remain / (*it)->time_unit(), timer);
                return;
            } else {
                timer->set_remain(timer->get_remain() + (*it)->get_passed_time());
            }
        }

        while (true) {
            auto wheel = std::make_unique<Wheel>(count_, time_unit);
            auto *wheel_ptr = ptr_of(wheel);
            wheels_.push_back(std::move(wheel));

            uint64_t remain = timer->get_remain();
            time_unit = wheel_ptr->unit_max_time();
            if (remain < time_unit) {
                wheel_ptr->place_timer(remain / wheel_ptr->time_unit(), timer);
                return;
            } else {
                timer->set_remain(timer->get_remain() + wheel_ptr->get_passed_time());
            }
        }
    }

    void WheelTimerManager::tick()
    {
        uint32_t click = base::time::get_passed_time();
        for (uint32_t time = 0; time < click; ++time) {
            const std::size_t wheel_count = wheels_.size();
            for (std::size_t i = 0; i < wheel_count; ++i) {
                Wheel *wheel = ptr_of(wheels_[i]);
                if (!wheel) {
                    continue;
                }

                uint64_t unit = wheel->time_unit();
                helper_item_.reset(wheel->tick(helper_item_.release()));

                while (helper_item_->begin() != helper_item_->end()) {
                    WheelTimer *timer = helper_item_->pop();
                    timer->set_remain(timer->get_remain() % unit);

                    if (timer->get_remain() < time_unit_) {
                        timer->trigger();
                        if (timer->ready()) {
                            place_timer(timer);
                        }
                    } else {
                        place_timer(timer);
                    }
                }

                helper_item_->reset();

                if (wheel->get_passed_time() != 0) {
                    break;
                }
            }
        }

        cleanup_finished_timers();
    }

    uint32_t WheelTimerManager::get_time_unit() const
    {
        return time_unit_;
    }

    const char *WheelTimerManager::backend_name() const
    {
        return "wheel";
    }

    uint32_t WheelTimerManager::get_poll_timeout_ms(uint32_t idle_timeout_ms, uint32_t active_timeout_cap_ms)
    {
        cleanup_finished_timers();
        if (!has_active_timers()) {
            return idle_timeout_ms;
        }

        const auto active_timeout = std::max<uint32_t>(1, time_unit_);
        return std::min(active_timeout, active_timeout_cap_ms == 0 ? active_timeout : active_timeout_cap_ms);
    }

    void WheelTimerManager::cleanup_finished_timers()
    {
        timers_.erase(
            std::remove_if(
                timers_.begin(),
                timers_.end(),
                [](const std::unique_ptr<WheelTimer> &timer) {
                    return !timer || timer->is_done() || timer->is_cancel();
                }),
            timers_.end());
    }

    bool WheelTimerManager::has_active_timers() const
    {
        return std::any_of(
            timers_.begin(),
            timers_.end(),
            [](const std::unique_ptr<WheelTimer> &timer) {
                return timer && !timer->is_done() && !timer->is_cancel();
            });
    }
}
