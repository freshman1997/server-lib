#include "eventbus/event_bus.h"
#include "server_runtime_host.h"

#include <any>
#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace
{
    bool require(bool condition, const std::string &message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }
}

int main()
{
    auto bus = std::make_shared<yuan::eventbus::EventBus>();
    std::optional<yuan::server::ServiceRuntimeEvent> activated;
    std::optional<yuan::server::ServiceRuntimeEvent> stopped;

    bus->subscribe(yuan::server::events::service_activated,
        [&](const yuan::eventbus::Event &event) {
            if (const auto *payload = std::any_cast<yuan::server::ServiceRuntimeEvent>(&event.payload)) {
                activated = *payload;
            }
        });
    bus->subscribe(yuan::server::events::service_stopped,
        [&](const yuan::eventbus::Event &event) {
            if (const auto *payload = std::any_cast<yuan::server::ServiceRuntimeEvent>(&event.payload)) {
                stopped = *payload;
            }
        });

    yuan::app::RuntimeContext context;
    context.app_name = "server-runtime-test";
    context.run_mode = yuan::app::RunMode::multi_process;
    context.worker_threads = 8;
    context.runtime_worker_count = 4;
    context.worker_index = 3;
    context.is_worker_process = true;
    context.active_service_name = "http";
    context.service_index = 9;
    context.service_instance_index = 3;
    context.service_instance_count = 4;
    context.listener_reuse_port = true;
    context.event_bus = bus;

    yuan::server::ServerRuntimeHost host({ "http", "http", 18080 });
    host.set_runtime_context(context);

    std::atomic_bool stop_requested{ false };
    if (!require(host.start([&]() {
            while (!stop_requested.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }),
        "server runtime host should start")) {
        return 1;
    }

    host.stop([&]() {
        stop_requested.store(true, std::memory_order_release);
    });

    if (!require(activated.has_value(), "server service activated event should be emitted")) {
        return 1;
    }
    if (!require(stopped.has_value(), "server service stopped event should be emitted")) {
        return 1;
    }
    if (!require(activated->app_name == "server-runtime-test" &&
                 activated->worker_threads == 8 &&
                 activated->runtime_worker_count == 4 &&
                 activated->worker_index == 3 &&
                 activated->is_worker_process,
                 "server service event should carry runtime worker identity")) {
        return 1;
    }
    if (!require(activated->service_name == "http" &&
                 activated->active_service_name == "http" &&
                 activated->service_index == 9 &&
                 activated->service_instance_index == 3 &&
                 activated->service_instance_count == 4 &&
                 activated->listener_reuse_port,
                 "server service event should carry service-instance identity")) {
        return 1;
    }
    if (!require(stopped->service_instance_index == 3 &&
                 stopped->service_instance_count == 4,
                 "server stopped event should preserve service-instance identity")) {
        return 1;
    }

    activated.reset();
    stopped.reset();
    context.active_service_name = "inline-http";
    context.service_index = 10;

    yuan::server::ServerRuntimeHost inline_host({ "inline-http", "http", 18081 });
    inline_host.set_runtime_context(context);

    const auto caller_thread = std::this_thread::get_id();
    std::thread::id start_fn_thread;
    bool start_fn_ran = false;
    if (!require(inline_host.start_inline([&]() {
            start_fn_thread = std::this_thread::get_id();
            start_fn_ran = true;
        }),
        "server runtime host should support inline start")) {
        return 1;
    }
    if (!require(start_fn_ran && start_fn_thread == caller_thread,
                 "inline start should run the start function on the caller thread")) {
        return 1;
    }
    if (!require(inline_host.is_started(), "inline host should stay started until stop")) {
        return 1;
    }
    if (!require(!inline_host.start_inline([]() {}), "inline host should reject duplicate start")) {
        return 1;
    }

    inline_host.stop();

    if (!require(activated.has_value() && activated->service_name == "inline-http",
                 "inline host should publish activated event")) {
        return 1;
    }
    if (!require(stopped.has_value() && stopped->service_name == "inline-http",
                 "inline host should publish stopped event")) {
        return 1;
    }
    if (!require(!inline_host.is_started(), "inline host should be stopped after stop")) {
        return 1;
    }

    std::cout << "server runtime host identity and inline lifecycle tests passed\n";
    return 0;
}
