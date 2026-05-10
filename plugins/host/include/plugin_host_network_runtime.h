#ifndef __YUAN_APP_PLUGIN_HOST_NETWORK_RUNTIME_H__
#define __YUAN_APP_PLUGIN_HOST_NETWORK_RUNTIME_H__

#include "plugin/host_network_runtime.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

namespace yuan::net
{
    class NetworkRuntime;
}

namespace yuan::app
{

    class PluginHostNetworkRuntime : public plugin::HostNetworkRuntime
    {
    public:
        explicit PluginHostNetworkRuntime(net::NetworkRuntime *runtime);
        ~PluginHostNetworkRuntime() override;

        bool is_available() const override;

        plugin::HostTimerId schedule_timer(uint32_t delay_ms, plugin::HostTimerCallback callback) override;

        plugin::HostTimerId schedule_periodic_timer(uint32_t delay_ms, uint32_t interval_ms,
                                                    plugin::HostTimerCallback callback, int repeat = 0) override;

        bool cancel_timer(plugin::HostTimerId timer_id) override;

        void dispatch(std::function<void()> callback) override;

        std::size_t worker_threads() const override;

        const char *runtime_name() const override;

    private:
        struct TimerRegistry;

        net::NetworkRuntime *runtime_;
        std::atomic<plugin::HostTimerId> next_id_{ 1 };
        std::shared_ptr<TimerRegistry> timer_registry_;
    };

} // namespace yuan::app

#endif
