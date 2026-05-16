#include "timer/heap_timer_manager.h"

#include "base/time.h"
#include "timer/basic_timer.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace yuan::timer
{
    HeapTimerManager::HeapTimerManager(uint32_t time_unit)
        : time_unit_(std::max<uint32_t>(1, time_unit)), sequence_(0)
    {
    }

    HeapTimerManager::~HeapTimerManager() = default;

    bool HeapTimerManager::LaterDeadline::operator()(const Entry &left, const Entry &right) const noexcept
    {
        if (left.deadline == right.deadline) {
            return left.sequence > right.sequence;
        }
        return left.deadline > right.deadline;
    }

    Timer *HeapTimerManager::timeout(uint32_t milliseconds, TimerTask *task)
    {
        return schedule(milliseconds, 0, task, 0);
    }

    Timer *HeapTimerManager::interval(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period)
    {
        return schedule(timeout, interval, task, period);
    }

    bool HeapTimerManager::schedule(Timer *timer)
    {
        auto *heap_timer = dynamic_cast<BasicTimer *>(timer);
        if (!heap_timer || heap_timer->is_cancel() || heap_timer->is_done()) {
            return false;
        }

        push(heap_timer);
        return true;
    }

    Timer *HeapTimerManager::schedule(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period)
    {
        auto owned_timer = std::make_unique<BasicTimer>(timeout, interval, task, period);
        BasicTimer *timer = &*owned_timer;
        timer->set_deadline(base::time::steady_now_ms() + timeout);
        timers_.push_back(std::move(owned_timer));
        push(timer);
        return timer;
    }

    void HeapTimerManager::push(BasicTimer *timer)
    {
        heap_.push(Entry{timer->deadline(), sequence_++, timer});
    }

    void HeapTimerManager::tick()
    {
        const uint64_t now = base::time::steady_now_ms();
        while (!heap_.empty()) {
            Entry entry = heap_.top();
            BasicTimer *timer = entry.timer;
            if (!timer || timer->is_cancel() || timer->is_done()) {
                heap_.pop();
                continue;
            }

            if (entry.deadline != timer->deadline()) {
                heap_.pop();
                continue;
            }

            if (entry.deadline > now) {
                break;
            }

            heap_.pop();
            const uint64_t next_deadline = timer->next_deadline(now);
            timer->trigger();
            if (timer->ready()) {
                timer->set_deadline(next_deadline);
                push(timer);
            }
        }

        cleanup_cancelled();
    }

    uint32_t HeapTimerManager::get_time_unit() const
    {
        return time_unit_;
    }

    const char *HeapTimerManager::backend_name() const
    {
        return "heap";
    }

    uint32_t HeapTimerManager::get_poll_timeout_ms(uint32_t idle_timeout_ms, uint32_t active_timeout_cap_ms)
    {
        cleanup_cancelled();
        while (!heap_.empty()) {
            const Entry &entry = heap_.top();
            BasicTimer *timer = entry.timer;
            if (!timer || timer->is_cancel() || timer->is_done() || entry.deadline != timer->deadline()) {
                heap_.pop();
                continue;
            }

            const uint64_t now = base::time::steady_now_ms();
            const uint64_t wait = entry.deadline > now ? entry.deadline - now : 0;
            const uint64_t cap = active_timeout_cap_ms == 0
                ? std::numeric_limits<uint32_t>::max()
                : active_timeout_cap_ms;
            return static_cast<uint32_t>(std::min<uint64_t>(wait, cap));
        }

        return idle_timeout_ms;
    }

    void HeapTimerManager::cleanup_cancelled()
    {
        std::unordered_set<BasicTimer *> live;
        std::unordered_set<BasicTimer *> expired;
        live.reserve(timers_.size());
        expired.reserve(timers_.size());
        for (const auto &timer : timers_) {
            if (timer) {
                live.insert(timer.get());
            }
            if (!timer || timer->is_done() || timer->is_cancel()) {
                expired.insert(timer.get());
            }
        }

        std::priority_queue<Entry, std::vector<Entry>, LaterDeadline> retained;
        while (!heap_.empty()) {
            Entry entry = heap_.top();
            heap_.pop();
            BasicTimer *timer = entry.timer;
            if (!timer || live.find(timer) == live.end() ||
                expired.find(timer) != expired.end() ||
                timer->is_done() || timer->is_cancel() ||
                entry.deadline != timer->deadline()) {
                continue;
            }

            retained.push(entry);
        }
        heap_.swap(retained);

        timers_.erase(
            std::remove_if(
                timers_.begin(),
                timers_.end(),
                [](const std::unique_ptr<BasicTimer> &timer) {
                    return !timer || timer->is_done() || timer->is_cancel();
                }),
            timers_.end());
    }
}
