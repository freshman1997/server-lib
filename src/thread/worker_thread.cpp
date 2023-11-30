#include "thread/task.h"
#include "thread/thread.h"
#include "thread/thread_pool.h"
#include "thread/worker_thread.h"

namespace thread
{
    WorkerThread::WorkerThread(ThreadPool *_pool, int id) : Thread(id), pool_(_pool)
    {
    }

    void WorkerThread::run()
    {
        if (!pool_) {
            return;
        }

        while (!stop_.load()) {
            Task *task = pool_->pop_task();
            if (!task) {
                stop_.store(true);
                break;
            }

            task->run();
        }
    }
}