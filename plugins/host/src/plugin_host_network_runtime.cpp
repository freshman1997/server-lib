#include "plugin_host_network_runtime.h"

#include "logger.h"
#include "net/runtime/network_runtime.h"
#include "timer/timer_handle.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace yuan::app
{
    struct PluginHostNetworkRuntime::TimerRegistry
    {
        mutable std::mutex mutex;
        std::unordered_map<plugin::HostTimerId, timer::TimerHandle> timer_handles;
    };

    PluginHostNetworkRuntime::PluginHostNetworkRuntime(net::NetworkRuntime * runtime)
        : runtime_(runtime)
        , timer_registry_(std::make_shared<TimerRegistry>())
    {
    }

    PluginHostNetworkRuntime::~PluginHostNetworkRuntime()
    {
        auto registry = std::move(timer_registry_);
        if (!registry) {
            return;
        }

        std::vector<timer::TimerHandle> timers;
        {
            std::lock_guard<std::mutex> lock(registry->mutex);
            timers.reserve(registry->timer_handles.size());
            for (auto &entry : registry->timer_handles) {
                timers.push_back(std::move(entry.second));
            }
            registry->timer_handles.clear();
        }

        for (const auto &timer : timers) {
            timer.cancel();
        }
    }

    bool PluginHostNetworkRuntime::is_available() const
    {
        return runtime_ != nullptr;
    }

    plugin::HostTimerId PluginHostNetworkRuntime::schedule_timer(uint32_t delay_ms,
                                                                 plugin::HostTimerCallback callback)
    {
        if (!runtime_ || !callback) {
            return 0;
        }

        const auto id = next_id_.fetch_add(1, std::memory_order_relaxed);
        auto registry = timer_registry_;
        if (!registry) {
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(registry->mutex);
            registry->timer_handles.emplace(id, timer::TimerHandle{});
        }

        std::weak_ptr<TimerRegistry> weak_registry = registry;
        auto timer = runtime_->schedule(delay_ms, [weak_registry, id, callback = std::move(callback)]() mutable {
            callback();
            if (auto locked = weak_registry.lock()) {
                std::lock_guard<std::mutex> lock(locked->mutex);
                locked->timer_handles.erase(id);
            }
        });
        if (!timer) {
            std::lock_guard<std::mutex> lock(registry->mutex);
            registry->timer_handles.erase(id);
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(registry->mutex);
            auto it = registry->timer_handles.find(id);
            if (it == registry->timer_handles.end()) {
                return id;
            }
            it->second = std::move(timer);
        }
        return id;
    }

    plugin::HostTimerId PluginHostNetworkRuntime::schedule_periodic_timer(
        uint32_t delay_ms, uint32_t interval_ms,
        plugin::HostTimerCallback callback, int repeat)
    {
        if (!runtime_ || !callback) {
            return 0;
        }

        const auto id = next_id_.fetch_add(1, std::memory_order_relaxed);
        auto registry = timer_registry_;
        if (!registry) {
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(registry->mutex);
            registry->timer_handles.emplace(id, timer::TimerHandle{});
        }

        std::weak_ptr<TimerRegistry> weak_registry = registry;
        auto remaining = repeat >= 0 ? std::make_shared<std::atomic<int>>(repeat > 0 ? repeat : 1) : nullptr;
        auto timer = runtime_->schedule_periodic(
            delay_ms,
            interval_ms,
            [weak_registry, id, callback = std::move(callback), remaining]() mutable {
                callback();
                if (remaining && remaining->fetch_sub(1, std::memory_order_relaxed) == 1) {
                    if (auto locked = weak_registry.lock()) {
                        std::lock_guard<std::mutex> lock(locked->mutex);
                        locked->timer_handles.erase(id);
                    }
                }
            },
            repeat);
        if (!timer) {
            std::lock_guard<std::mutex> lock(registry->mutex);
            registry->timer_handles.erase(id);
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(registry->mutex);
            auto it = registry->timer_handles.find(id);
            if (it == registry->timer_handles.end()) {
                return id;
            }
            it->second = std::move(timer);
        }
        return id;
    }

    bool PluginHostNetworkRuntime::cancel_timer(plugin::HostTimerId timer_id)
    {
        if (!runtime_ || timer_id == 0) {
            return false;
        }

        auto registry = timer_registry_;
        if (!registry) {
            return false;
        }

        timer::TimerHandle handle;
        {
            std::lock_guard<std::mutex> lock(registry->mutex);
            auto it = registry->timer_handles.find(timer_id);
            if (it == registry->timer_handles.end()) {
                return false;
            }
            handle = std::move(it->second);
            registry->timer_handles.erase(it);
        }

        handle.cancel();
        return true;
    }

    void PluginHostNetworkRuntime::dispatch(std::function<void()> callback)
    {
        if (!runtime_ || !callback) {
            return;
        }
        runtime_->dispatch(std::move(callback));
    }

    std::size_t PluginHostNetworkRuntime::worker_threads() const
    {
        return 1;
    }

    const char *PluginHostNetworkRuntime::runtime_name() const
    {
        return "NetworkRuntime";
    }

} // namespace yuan::app
