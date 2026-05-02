#include "coroutine/runtime_view.h"
#include "coroutine/sync_wait.h"
#include "coroutine/task.h"
#include "net/runtime/network_runtime.h"

#include <iostream>

namespace
{
    yuan::coroutine::Task<void> wait_for_timer(yuan::coroutine::RuntimeView runtime)
    {
        co_await runtime.sleep_for(50);
    }
}

int main()
{
    yuan::net::NetworkRuntime runtime;
    auto view = runtime.runtime_view().raw();

    bool fired = false;
    auto *timer = runtime.schedule(10, [&fired]() {
        fired = true;
    });

    if (!timer) {
        std::cerr << "schedule should return timer\n";
        return 1;
    }

    yuan::coroutine::sync_wait(view, wait_for_timer(view));

    if (!fired) {
        std::cerr << "one-shot timer should fire\n";
        return 1;
    }

    runtime.cancel_timer(timer);

    std::cout << "timer lifecycle test passed\n";
    return 0;
}
