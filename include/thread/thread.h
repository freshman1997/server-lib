#ifndef __THREAD_H__
#define __THREAD_H__
#include <string>
#include <thread>
#include <atomic>

#include "runnable.h"

namespace thread 
{
    class Thread : public Runnable
    {
    public:
        Thread(int id);

        ~Thread();

        void start();

        void join();

        void stop();

        void detach();

        bool is_stop() { return stop_.load() == true; }

        std::string get_thread_name()
        {
            return thread_name_;
        }

    private:
        void set_default_name();

    protected:
        std::atomic<bool> stop_{};
        bool join_;
        int tid_;
        std::thread *th;
        std::string thread_name_;
    };
}

#endif
