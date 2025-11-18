#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__
#include <cstddef>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>

namespace yuan::thread
{
    class Runnable;
    class WorkerThread;

    class ThreadPool
    {
        friend WorkerThread;
    public:
        ThreadPool();
        explicit ThreadPool(int thread_num);
        ~ThreadPool();

        void start() const;
        void push_task(Runnable *);
        Runnable * pop_task();

        std::string fetch_thread_status();
    private:
        void init();
        void run_worker(WorkerThread *);

    private:
        int thread_amount_;
        int timeout_{};
        std::size_t max_queue_size_{};
        std::deque<Runnable *> tasks_;
        std::vector<WorkerThread *> threads_;
        std::mutex mut_;
        std::condition_variable cond_;
    };
}

#endif
