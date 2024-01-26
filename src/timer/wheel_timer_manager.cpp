#include <cstdint>
#include <chrono>

#include "base/time.h"
#include "timer/wheel_timer_manager.h"
#include "timer/wheel_timer.h"

namespace timer 
{
    WheelTimerManager::WheelTimerManager()
    {
        count_ = 1000;
        time_unit_ = 10;
        helper_item_ = new WheelTimerItem;

        base::time::init_time(time_unit_);
    }

    WheelTimerManager::~WheelTimerManager() 
    {
        delete helper_item_;
        for (auto it = wheels_.begin(); it != wheels_.end(); ++it) {
            delete *it;
        }

        wheels_.clear();
    }

    Timer * WheelTimerManager::timeout(uint32_t milliseconds, TimerTask *task)
    {
        return schedule(milliseconds, 0, task, 0);
    }

    Timer * WheelTimerManager::interval(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period)
    {
        return schedule(timeout, interval, task, period);
    }

    bool WheelTimerManager::schedule(Timer *timer)
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

    Timer * WheelTimerManager::schedule(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period)
    {
        WheelTimer *timer = new WheelTimer(timeout, interval, task, period);
        place_timer(timer);
        return timer;
    }

    void WheelTimerManager::place_timer(WheelTimer *timer)
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
            Wheel *wheel = new Wheel(count_, time_unit);
            wheels_.push_back(wheel);

            uint64_t remain = timer->get_remain();
            time_unit = wheel->unit_max_time();
            if (remain < time_unit) {
                wheel->place_timer(remain / wheel->time_unit(), timer);
                return;
            } else {
                timer->set_remain(timer->get_remain() + wheel->get_passed_time());
            }
        }
    }
    
    void WheelTimerManager::tick()
    {
        uint32_t click = base::time::get_passed_time();
        for (uint32_t time = 0; time < click; ++time) {
            for (auto it = wheels_.begin(); it != wheels_.end(); ++it) {
                uint64_t unit = (*it)->time_unit();
                helper_item_ = (*it)->tick(helper_item_);

                while (helper_item_->begin() != helper_item_->end()) {
                    WheelTimer *timer = helper_item_->pop();
                    timer->set_remain(timer->get_remain() % unit);

                    if (timer->get_remain() < time_unit_) {
                        timer->trigger();
                        if (timer->is_cancel()) {
                            delete timer;
                        } else if (timer->ready()) {
                            place_timer(timer);
                        }
                    } else {
                        place_timer(timer);
                    }
                }

                helper_item_->reset();
                
                if ((*it)->get_passed_time() != 0) {
                    break;
                }
            }
        }
    }
}