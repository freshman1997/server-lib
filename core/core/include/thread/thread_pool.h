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

            auto task_ptr = std::make_shared<std::packaged_task<ReturnType()> >(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));

            std::future<ReturnType> fut = task_ptr->get_future();

            {
                std::unique_lock lock(mut_);
                if (max_queue_size_ > 0 && tasks_.size() >= max_queue_size_) {
                    handle_rejection([task_ptr]() { (*task_ptr)(); });
                    return fut;
                }
                tasks_.push_back([task_ptr]() { (*task_ptr)(); });
            }

            cond_.notify_one();
            return fut;
        }

        void push_task(std::unique_ptr<Runnable> task);

    private:
        void worker_loop();
        void handle_rejection(std::function<void()> task);

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
