#include "middleware.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    using yuan::net::http::FunctionMiddleware;
    using yuan::net::http::MiddlewarePipeline;
    using yuan::net::http::MiddlewareResult;

    void require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    void test_remove_and_owned_name()
    {
        MiddlewarePipeline pipeline;
        std::atomic<int> calls{0};
        std::string name = "temporary-name";
        auto middleware = std::make_shared<FunctionMiddleware>(
            [&calls](auto *, auto *) {
                ++calls;
                return MiddlewareResult::next;
            },
            name.c_str());
        const auto token = pipeline.add(middleware);
        name.assign("overwritten");

        require(token != 0, "add should return a token");
        require(std::string(middleware->name()) == "temporary-name", "middleware must own its name");
        require(pipeline.execute(nullptr, nullptr), "middleware pipeline should continue");
        require(calls.load() == 1, "registered middleware should execute");
        require(pipeline.remove(token), "remove should unregister an existing token");
        require(!pipeline.remove(token), "remove should reject an already removed token");
        require(pipeline.execute(nullptr, nullptr), "empty middleware pipeline should continue");
        require(calls.load() == 1, "removed middleware must not execute");
    }

    void test_remove_waits_for_in_flight()
    {
        MiddlewarePipeline pipeline;
        bool entered = false;
        bool release = false;
        std::mutex gate_mutex;
        std::condition_variable gate_cv;
        const auto token = pipeline.add([&](auto *, auto *) {
            std::unique_lock<std::mutex> lock(gate_mutex);
            entered = true;
            gate_cv.notify_all();
            gate_cv.wait(lock, [&]() { return release; });
            return MiddlewareResult::next;
        });

        std::thread executor([&]() { pipeline.execute(nullptr, nullptr); });
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            gate_cv.wait(lock, [&]() { return entered; });
        }
        std::atomic<bool> removed{false};
        std::thread removal([&]() { removed.store(pipeline.remove(token), std::memory_order_release); });
        std::this_thread::sleep_for(50ms);
        require(!removed.load(std::memory_order_acquire), "remove must wait for an in-flight middleware");
        {
            std::lock_guard<std::mutex> lock(gate_mutex);
            release = true;
        }
        gate_cv.notify_all();
        removal.join();
        require(removed.load(std::memory_order_acquire), "remove should finish after the middleware exits");
        executor.join();
    }

    void test_self_unregister_does_not_deadlock()
    {
        MiddlewarePipeline pipeline;
        std::atomic<int> calls{0};
        uint64_t token = 0;
        token = pipeline.add([&](auto *, auto *) {
            ++calls;
            require(pipeline.remove(token), "self-unregister should remove its token");
            return MiddlewareResult::next;
        });

        require(pipeline.execute(nullptr, nullptr), "self-unregister must not deadlock");
        pipeline.execute(nullptr, nullptr);
        require(calls.load() == 1, "self-unregistered middleware must not run again");
    }

    void test_concurrent_registration_execution_and_removal()
    {
        MiddlewarePipeline pipeline;
        std::atomic<bool> stop{false};
        std::atomic<int> calls{0};
        std::thread executor([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                pipeline.execute(nullptr, nullptr);
            }
        });

        for (int i = 0; i < 500; ++i) {
            const auto token = pipeline.add([&calls](auto *, auto *) {
                ++calls;
                return MiddlewareResult::next;
            });
            require(token != 0, "concurrent add should return a token");
            require(pipeline.remove(token), "concurrent remove should find its token");
        }
        stop.store(true, std::memory_order_relaxed);
        executor.join();
        require(pipeline.empty(), "all concurrently registered middleware should be removed");
    }
}

int main()
{
    test_remove_and_owned_name();
    test_remove_waits_for_in_flight();
    test_self_unregister_does_not_deadlock();
    test_concurrent_registration_execution_and_removal();
    return 0;
}
