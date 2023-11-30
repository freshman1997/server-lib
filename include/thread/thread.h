#ifndef __THREAD_H__
#define __THREAD_H__
#include <string>
#include <thread>
#include <atomic>

#include "thread/task.h"
namespace thread 
{
    class Thread : public Task
    {
    public:
        Thread(int id);

        void start();

        void join();

        void stop();

        void detach();

        bool is_stop() { return stop_.load() == true; }

    private:
        void set_default_name();

    protected:
        std::atomic<bool> stop_;
        bool join_;
        int tid_;
        std::thread *th;
        std::string thread_name_;
    };
}

#endif
