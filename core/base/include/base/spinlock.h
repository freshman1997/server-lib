#ifndef __YUAN_BASE_SPINLOCK_H__
#define __YUAN_BASE_SPINLOCK_H__

#include <atomic>

namespace yuan::base
{
    class Spinlock
    {
    public:
        Spinlock() = default;
        Spinlock(const Spinlock &) = delete;
        Spinlock &operator=(const Spinlock &) = delete;

        void lock() noexcept
        {
            while (flag_.test_and_set(std::memory_order_acquire))
            {
            }
        }

        bool try_lock() noexcept
        {
            return !flag_.test_and_set(std::memory_order_acquire);
        }

        void unlock() noexcept
        {
            flag_.clear(std::memory_order_release);
        }

    private:
        std::atomic_flag flag_{};
    };
}

#endif
