#include "timer/timer_manager.h"

#include <utility>

namespace
{
    class SimpleTimerTask : public yuan::timer::TimerTask
    {
    public:
        explicit SimpleTimerTask(std::function<void()> func)
            : func_(std::move(func))
        {
        }

        void on_timer(yuan::timer::Timer *timer) override
        {
            (void)timer;
            func_();
        }

        bool need_free() const override
        {
            return true;
        }

    private:
        std::function<void()> func_;
    };
}

namespace yuan::timer
{
    TimerHandle TimerManager::after(uint32_t milliseconds, std::function<void()> callback)
    {
        if (!callback) {
            return {};
        }
        return after(milliseconds, new SimpleTimerTask(std::move(callback)));
    }

    TimerHandle TimerManager::every(uint32_t timeout, uint32_t interval, std::function<void()> callback, int32_t period)
    {
        if (!callback) {
            return {};
        }
        return every(timeout, interval, new SimpleTimerTask(std::move(callback)), period);
    }
}
