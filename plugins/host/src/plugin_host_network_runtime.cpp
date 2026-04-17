#include "plugin_host_network_runtime.h"

#include "logger.h"
#include "net/runtime/network_runtime.h"
#include "timer/timer.h"

namespace yuan::app
{

    PluginHostNetworkRuntime::PluginHostNetworkRuntime(net::NetworkRuntime * runtime)
        : runtime_(runtime)
    {
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

        auto id = next_id_.fetch_add(1);
        auto *timer = runtime_->schedule(delay_ms, std::move(callback));
        if (!timer) {
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            timer_handles_[id] = timer;
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

        auto id = next_id_.fetch_add(1);
        auto *timer = runtime_->schedule_periodic(delay_ms, interval_ms, std::move(callback), repeat);
        if (!timer) {
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            timer_handles_[id] = timer;
        }
        return id;
    }

    bool PluginHostNetworkRuntime::cancel_timer(plugin::HostTimerId timer_id)
    {
        if (!runtime_ || timer_id == 0) {
            return false;
        }

        void *handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = timer_handles_.find(timer_id);
            if (it == timer_handles_.end()) {
                return false;
            }
            handle = it->second;
            timer_handles_.erase(it);
        }

        runtime_->cancel_timer(static_cast<yuan::timer::Timer *>(handle));
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
