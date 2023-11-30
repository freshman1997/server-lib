#ifndef __THREAD_H__
#define __THREAD_H__
#include <deque>
#include <string>
#include <thread>
#include <atomic>

namespace thread 
{
    class Task;

    class Thread
    {
    public:
        Thread(int id);

        virtual void run() = 0;

        void start();

        void join();

        void stop();

        void detach();

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
