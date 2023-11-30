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

        virtual void run();

    private:
        ThreadPool *pool_;
    };
}
#endif
