#ifndef __EVENT_LOOP_THREAD_H__
#define __EVENT_LOOP_THREAD_H__

#include "thread/worker_thread.h"

namespace thread 
{
    class EventLoop;
    class EventLoopThread : public thread::WorkerThread
    {

    protected:
        EventLoopThread(EventLoop *loop, int tid);
        virtual void run_internal();


    private:
        EventLoop *loop_;

    };
}

#endif
