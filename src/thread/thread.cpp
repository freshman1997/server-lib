#include <atomic>
#include <cassert>
#include <thread>

#include "thread/thread.h"

namespace thread
{
    Thread::Thread(const int id) : Runnable(true), join_(false), tid_(id), th(nullptr)
    {
        stop_.store(false);
    }

    Thread::~Thread()
    {
        if (th) {
            if (!join_) {
                join();
            }
            delete th;
        }
    }

    void Thread::start()
    {
        assert(th == nullptr);
        set_default_name();
        th = new std::thread([this] { run(); });
    }

    void Thread::join()
    {
        assert(th != nullptr && !join_);
        if (th->joinable() && !join_) {
            th->join();
        }
    }

    void Thread::stop()
    {
        assert(th != nullptr);
        stop_.store(true, std::memory_order_release);
    }

    void Thread::detach()
    {
        assert(th != nullptr);
        th->detach();
    }

    void Thread::set_default_name()
    {
        this->thread_name_ = "thread_" + std::to_string(tid_);
    }
}
