#ifndef __YUAN_PLUGIN_HOST_NETWORK_RUNTIME_H__
#define __YUAN_PLUGIN_HOST_NETWORK_RUNTIME_H__

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace yuan::plugin
{

    using HostTimerId = std::uint64_t;
    using HostTimerCallback = std::function<void()>;

    class HostNetworkRuntime
    {
    public:
        virtual ~HostNetworkRuntime() = default;

        virtual bool is_available() const = 0;

        virtual HostTimerId schedule_timer(uint32_t delay_ms, HostTimerCallback callback) = 0;

        virtual HostTimerId schedule_periodic_timer(uint32_t delay_ms, uint32_t interval_ms,
                                                    HostTimerCallback callback, int repeat = 0) = 0;

        virtual bool cancel_timer(HostTimerId timer_id) = 0;

        virtual void dispatch(std::function<void()> callback) = 0;

        virtual std::size_t worker_threads() const = 0;

        virtual const char *runtime_name() const = 0;
    };

} // namespace yuan::plugin

#endif
