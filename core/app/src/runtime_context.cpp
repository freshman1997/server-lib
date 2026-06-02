#include "runtime_context.h"

#include <thread>

namespace yuan::app
{

std::size_t normalized_worker_count(const std::size_t worker_count) noexcept
{
    return worker_count == 0 ? 1 : worker_count;
}

void normalize_runtime_context(RuntimeContext &context)
{
    switch (context.run_mode) {
    case RunMode::single_thread:
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        break;
    case RunMode::multi_thread:
        if (context.worker_threads < 2) {
            const auto hardware_threads = std::thread::hardware_concurrency();
            context.worker_threads = hardware_threads > 1 ? hardware_threads : 2;
        }
        if (context.runtime_workers.worker_count == 0) {
            context.runtime_workers.worker_count = context.worker_threads;
        }
        context.runtime_worker_count = context.runtime_workers.worker_count;
        break;
    case RunMode::multi_process:
        context.worker_threads = normalized_worker_count(context.worker_threads);
        if (context.runtime_workers.worker_count == 0) {
            context.runtime_workers.worker_count = context.worker_threads;
        }
        context.runtime_worker_count = context.runtime_workers.worker_count;
        break;
    default:
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        break;
    }
}

} // namespace yuan::app
