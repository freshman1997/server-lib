#ifndef __WORKER_THREAD_H__
#define __WORKER_THREAD_H__
#include "thread/thread.h"

namespace thread 
{
    class ThreadPool;

    class WorkerThread : public Thread
    {
    public:
        WorkerThread(ThreadPool *_pool, int id);

    protected:
        virtual void run_internal();

    private:
        ThreadPool *pool_;
    };
}
#endif
