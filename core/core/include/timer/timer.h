#ifndef __TIMER_H__
#define __TIMER_H__

#include <functional>
#include <memory>
#include <mutex>

namespace yuan::timer
{
    class TimerHandleState
    {
    public:
        TimerHandleState() = default;
        TimerHandleState(const TimerHandleState &) = delete;
        TimerHandleState &operator=(const TimerHandleState &) = delete;

        void cancel() noexcept
        {
            std::function<void()> cancel_fn;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cancel_fn = std::move(cancel_fn_);
                active_ = false;
            }

            if (cancel_fn) {
                cancel_fn();
            }
        }

        bool active() const noexcept
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return active_;
        }

        void bind(std::function<void()> cancel_fn)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancel_fn_ = std::move(cancel_fn);
            active_ = static_cast<bool>(cancel_fn_);
        }

        void clear() noexcept
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancel_fn_ = nullptr;
            active_ = false;
        }

    private:
        mutable std::mutex mutex_;
        std::function<void()> cancel_fn_;
        bool active_ = false;
    };

    enum class TimerState : char {
        init = 0,
        processing,
        done,
        cancal,
    };

    class TimerTask;
    class Timer
    {
    public:
        virtual ~Timer()
        {
        }

        virtual bool ready() const = 0;

        virtual void cancel() = 0;

        virtual void reset() = 0;

        virtual bool is_processing() const = 0;

        virtual bool is_done() const = 0;

        virtual bool is_cancel() const = 0;

        virtual TimerTask *get_task() const = 0;

        virtual std::shared_ptr<TimerHandleState> handle_state() const
        {
            return {};
        }
    };
}

#endif
