#include "thread/thread_pool.h"
#include "thread/worker_thread.h"

namespace yuan::thread 
{
    ThreadPool::ThreadPool() : ThreadPool(2)
    {
    }

    ThreadPool::ThreadPool(int thread_num) : thread_amount_(thread_num)
    {
        init();
    }

    ThreadPool::~ThreadPool()
    {
        cond_.notify_all();
        for (auto &it : threads_) {
            it->stop();
        }
        
        for (auto &it : tasks_) {
            delete it;
        }
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

    void ThreadPool::push_task(Runnable *task)
    {
        std::unique_lock<std::mutex> lock(mut_);
        this->tasks_.push_back(task);

        cond_.notify_one();
    }

    Runnable * ThreadPool::pop_task()
    {
        std::unique_lock<std::mutex> lock(mut_);
        
        if (tasks_.empty()) {
            return nullptr;
        }

        Runnable *task = tasks_.back();
        tasks_.pop_back();

        return task;
    }

    void ThreadPool::run_worker(WorkerThread *thread)
    {
        if (!thread) {
            return;
        }

        while (!thread->is_stop()) {
            Runnable *task = pop_task();
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