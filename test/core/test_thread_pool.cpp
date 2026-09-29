#include "thread/thread_pool.h"

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    bool future_ready(std::future<yuan::thread::ThreadTaskResult<int>> &future)
    {
        return future.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    }

    bool expect_runtime_error(std::future<yuan::thread::ThreadTaskResult<int>> &future, const char *message)
    {
        auto result = future.get();
        if (result.error) {
            return true;
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
        auto running_result = running.get();
        auto queued_result = queued.get();
        if (!running_result || !queued_result || *running_result.value != 1 || *queued_result.value != 2) {
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
        auto rejected = pool.submit([] { return 3; });
        if (!future_ready(rejected) || !expect_runtime_error(rejected, "abort rejection should complete future with error")) {
            blocker.set_value();
            pool.shutdown();
            return 1;
        }
        blocker.set_value();
        auto running_result = running.get();
        auto queued_result = queued.get();
        if (!running_result || !queued_result || *running_result.value != 1 || *queued_result.value != 2) {
            std::cerr << "accepted tasks did not run after abort rejection\n";
            pool.shutdown();
            return 1;
        }
        pool.shutdown();
    }

    {
        yuan::thread::ThreadPool pool(1);
        pool.start();
        auto successful = pool.submit([] {});
        auto failed = pool.submit([] { throw std::runtime_error("task failed"); });
        if (!successful.get() || failed.get()) {
            std::cerr << "void tasks should report success and failure without throwing\n";
            pool.shutdown();
            return 1;
        }
        pool.shutdown();
    }

    {
        yuan::thread::ThreadPool pool(1);
        pool.start();
        std::promise<void> blocker;
        std::promise<void> started;
        auto blocker_future = blocker.get_future();
        auto started_future = started.get_future();
        auto running = pool.submit([&] {
            started.set_value();
            blocker_future.wait();
        });
        started_future.wait();
        auto cancelled = pool.submit([] { return 42; });
        std::thread stopper([&] { pool.stop(); });
        if (!future_ready(cancelled) || cancelled.get()) {
            std::cerr << "stop should complete queued tasks with cancellation\n";
            blocker.set_value();
            stopper.join();
            return 1;
        }
        blocker.set_value();
        stopper.join();
        if (!running.get()) {
            std::cerr << "running task should finish during stop\n";
            return 1;
        }
    }

    std::cout << "thread pool test passed\n";
    return 0;
}
