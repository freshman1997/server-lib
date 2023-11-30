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
    class Thread;

    class ThreadPool
    {
    public:
        ThreadPool();
        ThreadPool(int thread_num);

        void start();
        void push_task(Task *);
        Task * pop_task();

        std::string fetch_thread_status();
    private:
        void init();
        static const int default_thread_num;

    private:
        int thread_amount_;
        int timeout_;
        std::size_t max_queue_size_;
        std::deque<Task *> tasks_;
        std::vector<Thread *> threads_;
        std::mutex mut_;
        std::condition_variable cond_;
    };
}

#endif
