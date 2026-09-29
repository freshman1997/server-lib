#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
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

    template <typename T>
    struct ThreadTaskResult {
        std::optional<T> value;
        std::exception_ptr error;
        explicit operator bool() const noexcept { return !error && value.has_value(); }
    };

    template <>
    struct ThreadTaskResult<void> {
        bool completed = false;
        std::exception_ptr error;
        explicit operator bool() const noexcept { return completed && !error; }
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
        auto submit(F &&f, Args &&... args) -> std::future<ThreadTaskResult<std::invoke_result_t<F, Args...> > >
        {
            using ReturnType = std::invoke_result_t<F, Args...>;
            using ResultType = ThreadTaskResult<ReturnType>;

            auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
            auto promise = std::make_shared<std::promise<ResultType> >();
            std::future<ResultType> fut = promise->get_future();
            auto task = [promise, bound = std::move(bound)](bool cancelled) mutable {
                ResultType result;
                if (cancelled) {
                    result.error = std::make_exception_ptr(std::runtime_error("thread pool task cancelled"));
                } else {
                    try {
                        if constexpr (std::is_void_v<ReturnType>) {
                            bound();
                            result.completed = true;
                        } else {
                            result.value = bound();
                        }
                    } catch (...) {
                        result.error = std::current_exception();
                    }
                }
                promise->set_value(std::move(result));
            };

            {
                std::unique_lock lock(mut_);
                if (!running_.load(std::memory_order_acquire)) {
                    ResultType rejected;
                    rejected.error = std::make_exception_ptr(std::runtime_error("thread pool is not running"));
                    promise->set_value(std::move(rejected));
                    return fut;
                }

                if (max_queue_size_ > 0 && tasks_.size() >= max_queue_size_) {
                    if (reject_policy_ == RejectPolicy::caller_runs) {
                        lock.unlock();
                        task(false);
                    } else {
                        ResultType rejected;
                        rejected.error = std::make_exception_ptr(std::runtime_error("thread pool queue full"));
                        promise->set_value(std::move(rejected));
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
        
    private:
        int thread_count_;
        std::size_t max_queue_size_;
        RejectPolicy reject_policy_;
        std::atomic<bool> running_{};
        std::atomic<std::size_t> active_count_{};
        std::deque<std::function<void(bool)> > tasks_;
        std::vector<std::thread> threads_;
        std::mutex mut_;
        std::condition_variable cond_;
        std::condition_variable done_;
    };
}

#endif
