#ifndef __YUAN_APP_METRICS_RESOURCE_USAGE_REPORTER_H__
#define __YUAN_APP_METRICS_RESOURCE_USAGE_REPORTER_H__

#include "timer/timer_handle.h"

#include <cstdint>
#include <string>

namespace yuan::timer
{
    class TimerManager;
}

namespace yuan::app::metrics
{
    struct ResourceUsageReportOptions
    {
        bool enabled = true;
        std::uint32_t interval_ms = 60000;
        std::string name;
    };

    class ResourceUsageReporter
    {
    public:
        ResourceUsageReporter() = default;
        ~ResourceUsageReporter();

        ResourceUsageReporter(const ResourceUsageReporter &) = delete;
        ResourceUsageReporter &operator=(const ResourceUsageReporter &) = delete;

        void start(timer::TimerManager &timer_manager, ResourceUsageReportOptions options = {});
        void stop();
        [[nodiscard]] bool running() const noexcept;

    private:
        timer::TimerHandle timer_;
        bool owns_process_reporter_ = false;
    };

    void start_process_resource_usage_reporter(timer::TimerManager &timer_manager, ResourceUsageReportOptions options = {});
    void stop_process_resource_usage_reporter();
}

#endif
