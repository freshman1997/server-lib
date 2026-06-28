#include "thread/thread_pool.h"

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    bool future_ready(std::future<int> &future)
    {
        return future.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    }

    bool expect_runtime_error(std::future<int> &future, const char *message)
    {
        try {
            (void)future.get();
        } catch (const std::runtime_error &) {
            return true;
        } catch (...) {
        }
        std::cerr << message << '\n';
        return false;
    }
}

int main()
{
    {
        yuan::thread::ThreadPool pool(1);
        auto future = pool.submit([] { return 1; });
        if (!future_ready(future) || !expect_runtime_error(future, "submit before start should fail")) {
            return 1;
        }
    }

    {
        yuan::thread::ThreadPool pool(1);
        pool.start();
        pool.stop();
        auto future = pool.submit([] { return 1; });
        if (!future_ready(future) || !expect_runtime_error(future, "submit after stop should fail")) {
            return 1;
        }
    }

    {
        yuan::thread::ThreadPool pool({ 1, 1, yuan::thread::RejectPolicy::discard });
        pool.start();
        std::promise<void> blocker;
        std::promise<void> started;
        auto blocker_future = blocker.get_future();
        auto started_future = started.get_future();
        auto running = pool.submit([&blocker_future, &started] {
            started.set_value();
            blocker_future.wait();
            return 1;
        });
        started_future.wait();
        auto queued = pool.submit([] { return 2; });
        auto rejected = pool.submit([] { return 3; });
        if (!future_ready(rejected) || !expect_runtime_error(rejected, "discard rejection should complete future with error")) {
            blocker.set_value();
            pool.shutdown();
            return 1;
        }
        blocker.set_value();
        if (running.get() != 1 || queued.get() != 2) {
            std::cerr << "accepted tasks did not run\n";
            pool.shutdown();
            return 1;
        }
        pool.shutdown();
    }

    {
        yuan::thread::ThreadPool pool({ 1, 1, yuan::thread::RejectPolicy::abort });
        pool.start();
        std::promise<void> blocker;
        std::promise<void> started;
        auto blocker_future = blocker.get_future();
        auto started_future = started.get_future();
        auto running = pool.submit([&blocker_future, &started] {
            started.set_value();
            blocker_future.wait();
            return 1;
        });
        started_future.wait();
        auto queued = pool.submit([] { return 2; });
        try {
            auto rejected = pool.submit([] { return 3; });
            (void)rejected;
            std::cerr << "abort rejection should throw\n";
            blocker.set_value();
            pool.shutdown();
            return 1;
        } catch (const std::runtime_error &) {
        }
        blocker.set_value();
        if (running.get() != 1 || queued.get() != 2) {
            std::cerr << "accepted tasks did not run after abort rejection\n";
            pool.shutdown();
            return 1;
        }
        pool.shutdown();
    }

    std::cout << "thread pool test passed\n";
    return 0;
}
