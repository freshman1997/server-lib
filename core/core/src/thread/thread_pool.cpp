#include "thread/runnable.h"
#include "thread/thread_pool.h"
#include "logger.h"

#include <exception>

namespace yuan::thread
{
    ThreadPool::ThreadPool()
        : ThreadPool(ThreadPoolConfig{})
    {
    }

    ThreadPool::ThreadPool(const int thread_num)
        : ThreadPool(ThreadPoolConfig{ thread_num > 0 ? thread_num : 1, 0, RejectPolicy::abort })
    {
    }

    ThreadPool::ThreadPool(ThreadPoolConfig config)
        : thread_count_(config.thread_count > 0 ? config.thread_count : 1), max_queue_size_(config.max_queue_size), reject_policy_(config.reject_policy)
    {
        running_.store(false, std::memory_order_relaxed);
        active_count_.store(0, std::memory_order_relaxed);
    }

    ThreadPool::~ThreadPool()
    {
        stop();
    }

    void ThreadPool::start()
    {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        for (int i = 0; i < thread_count_; ++i) {
            threads_.emplace_back([this] { worker_loop(); });
        }
    }

    void ThreadPool::stop()
    {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return;
        }

        {
            std::unique_lock lock(mut_);
            tasks_.clear();
        }
        cond_.notify_all();

        for (auto &t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }

    void ThreadPool::shutdown()
    {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return;
        }

        cond_.notify_all();

        for (auto &t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }

    void ThreadPool::wait_all()
    {
        std::unique_lock lock(mut_);
        done_.wait(lock, [this] {
            return tasks_.empty() && active_count_.load(std::memory_order_acquire) == 0;
        });
    }

    void ThreadPool::push_task(std::unique_ptr<Runnable> task)
    {
        if (!task) {
            return;
        }

        auto shared_task = std::shared_ptr<Runnable>(std::move(task));
        submit([shared_task] {
            shared_task->run();
        });
    }

    void ThreadPool::handle_rejection(std::promise<void> &promise)
    {
        switch (reject_policy_) {
        case RejectPolicy::discard:
            set_rejected_exception(promise, "thread pool queue full");
            break;
        case RejectPolicy::abort:
        default:
            set_rejected_exception(promise, "thread pool queue full");
            throw std::runtime_error("thread pool queue full");
        }
    }

    void ThreadPool::worker_loop()
    {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mut_);
                cond_.wait(lock, [this] {
                    return !tasks_.empty() || !running_.load(std::memory_order_acquire);
                });

                if (!tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                } else if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
            }

            if (task) {
                active_count_.fetch_add(1, std::memory_order_relaxed);
                try
                {
                    task();
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR("Exception in thread pool task: {}", e.what());
                }
                catch (...)
                {
                    LOG_ERROR("Unknown exception in thread pool task");
                }
                active_count_.fetch_sub(1, std::memory_order_relaxed);
                done_.notify_all();
            }
        }
    }
}
