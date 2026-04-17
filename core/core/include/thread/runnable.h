#ifndef __RUNNABLE_H__
#define __RUNNABLE_H__

#include <atomic>

namespace yuan::thread
{
    class Runnable
    {
    public:
        Runnable()
            : valid_(true)
        {
        }
        explicit Runnable(bool valid)
            : valid_(valid)
        {
        }
        virtual ~Runnable() = default;

        void cancel_task()
        {
            valid_.store(false, std::memory_order_release);
        }
        void enable_task()
        {
            valid_.store(true, std::memory_order_release);
        }
        bool is_valid() const
        {
            return valid_.load(std::memory_order_acquire);
        }

        void run()
        {
            if (valid_.load(std::memory_order_acquire)) {
                run_internal();
            }
        }

    protected:
        virtual void run_internal() = 0;

    private:
        std::atomic<bool> valid_;
    };
}

#endif
