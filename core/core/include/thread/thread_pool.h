#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace yuan::thread
{
    class Runnable;

    enum class RejectPolicy {
        abort,
        discard,
        caller_runs
    };

    struct ThreadPoolConfig
    {
        int thread_count = 2;
        std::size_t max_queue_size = 0;
        RejectPolicy reject_policy = RejectPolicy::abort;
    };

    class ThreadPool
    {
    public:
        ThreadPool();
        explicit ThreadPool(int thread_num);
        explicit ThreadPool(ThreadPoolConfig config);
        ~ThreadPool();

        void start();
        void stop();
        void shutdown();
        void wait_all();

        template <typename F, typename... Args>
        auto submit(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, Args...> >
        {
            using ReturnType = std::invoke_result_t<F, Args...>;

            auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
            auto promise = std::make_shared<std::promise<ReturnType> >();
            std::future<ReturnType> fut = promise->get_future();
            auto task = [promise, bound = std::move(bound)]() mutable {
                try {
                    if constexpr (std::is_void_v<ReturnType>) {
                        bound();
                        promise->set_value();
                    } else {
                        promise->set_value(bound());
                    }
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            };

            {
                std::unique_lock lock(mut_);
                if (!running_.load(std::memory_order_acquire)) {
                    set_rejected_exception(*promise, "thread pool is not running");
                    return fut;
                }

                if (max_queue_size_ > 0 && tasks_.size() >= max_queue_size_) {
                    if (reject_policy_ == RejectPolicy::caller_runs) {
                        lock.unlock();
                        task();
                    } else {
                        handle_rejection(*promise);
                    }
                    return fut;
                }
                tasks_.push_back(std::move(task));
            }

            cond_.notify_one();
            return fut;
        }

        void push_task(std::unique_ptr<Runnable> task);

    private:
        void worker_loop();
        void handle_rejection(std::promise<void> &promise);

        template <typename ReturnType>
        void handle_rejection(std::promise<ReturnType> &promise)
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

        template <typename ReturnType>
        static void set_rejected_exception(std::promise<ReturnType> &promise, const char *message)
        {
            try {
                throw std::runtime_error(message);
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        }

    private:
        int thread_count_;
        std::size_t max_queue_size_;
        RejectPolicy reject_policy_;
        std::atomic<bool> running_{};
        std::atomic<std::size_t> active_count_{};
        std::deque<std::function<void()> > tasks_;
        std::vector<std::thread> threads_;
        std::mutex mut_;
        std::condition_variable cond_;
        std::condition_variable done_;
    };
}

#endif
