#include "thread/thread_pool.h"
#include "thread/worker_thread.h"

namespace thread 
{
    const int ThreadPool::default_thread_num = 1;

    ThreadPool::ThreadPool() : thread_amount_(default_thread_num)
    {
        init();
    }

    ThreadPool::ThreadPool(int thread_num) : thread_amount_(thread_num)
    {
        init();
    }

    ThreadPool::~ThreadPool()
    {
        for (auto &it : threads_) {
            it->stop();
        }
        
        cond_.notify_all();
    }

    void ThreadPool::init()
    {
        for (int i = 0; i < thread_amount_; ++i) {
            threads_.push_back(new WorkerThread(this, i));
        }
    }

    void ThreadPool::start()
    {
        for (auto &it : threads_) {
            it->start();
        }
    }

    void ThreadPool::push_task(Task *task)
    {
        std::unique_lock<std::mutex> lock(mut_);
        this->tasks_.push_back(task);

        cond_.notify_one();
    }

    Task * ThreadPool::pop_task()
    {
        std::unique_lock<std::mutex> lock(mut_);
        
        if (tasks_.empty()) {
            return nullptr;
        }

        Task *task = tasks_.back();
        tasks_.pop_back();

        return task;
    }

    void ThreadPool::run_worker(WorkerThread *thread)
    {
        if (!thread) {
            return;
        }

        while (!thread->is_stop()) {
            Task *task = pop_task();
            if (!task) {
                std::unique_lock<std::mutex> lock(mut_);
                while (tasks_.empty() && !thread->is_stop()) {
                    cond_.wait(lock);
                }

                continue;
            }

            task->run();
            delete task;
        }
    }

    std::string ThreadPool::fetch_thread_status()
    {
        return "";
    }
}