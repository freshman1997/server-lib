#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__
#include <cstddef>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>

namespace thread
{
    class Task;
    class WorkerThread;

    class ThreadPool
    {
        friend WorkerThread;
    public:
        ThreadPool();
        ThreadPool(int thread_num);
        ~ThreadPool();

        void start();
        void push_task(Task *);
        Task * pop_task();

        std::string fetch_thread_status();
    private:
        void init();
        void run_worker(WorkerThread *);

    private:
        int thread_amount_;
        int timeout_;
        std::size_t max_queue_size_;
        std::deque<Task *> tasks_;
        std::vector<WorkerThread *> threads_;
        std::mutex mut_;
        std::condition_variable cond_;
    };
}

#endif
