#include "thread/thread_pool.h"
#include "thread/thread.h"

namespace thread 
{
    const int ThreadPool::default_thread_num = 1;

    ThreadPool::ThreadPool() : thread_amount_(default_thread_num)
    {

    }

    ThreadPool::ThreadPool(int thread_num) : thread_amount_(thread_num)
    {

    }

    void ThreadPool::init()
    {
        for (int i = 0; i < thread_amount_; ++i) {
            //threads_.push_back();
        }
    }

    void ThreadPool::start()
    {

    }

    void ThreadPool::push_task(Task *task)
    {
        std::unique_lock<std::mutex> lock(mut_);
        this->tasks_.push_back(task);
    }

    Task * ThreadPool::pop_task()
    {
        std::unique_lock<std::mutex> lock(mut_);

        return nullptr;
    }


    std::string ThreadPool::fetch_thread_status()
    {
        return "";
    }
}