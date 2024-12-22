#include "thread/thread.h"
#include "thread/thread_pool.h"
#include "thread/worker_thread.h"

namespace yuan::thread
{
    WorkerThread::WorkerThread(ThreadPool *_pool, int id) : Thread(id), pool_(_pool)
    {
        
    }

    void WorkerThread::run_internal()
    {
        if (!pool_) {
            return;
        }

        pool_->run_worker(this);
        delete this;
    }
}