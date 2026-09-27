#ifndef __SERVICE_H__
#define __SERVICE_H__

#include "runtime_context.h"

namespace yuan::timer
{
    class TimerManager;
}

namespace yuan::app
{
    class Service
    {
    public:
        virtual ~Service() = default;

        virtual bool init() = 0;

        virtual void start() = 0;

        // Runs the service's foreground event loop when it owns one.
        // Services that already run asynchronously from start() keep the default.
        virtual bool run() { return true; }

        // Returns true only after the service has completed its shutdown.
        // A false result keeps the service's event loop and dependencies alive.
        virtual bool stop() = 0;

        virtual timer::TimerManager *resource_usage_timer_manager() { return nullptr; }

        virtual const char *resource_usage_report_name() const { return nullptr; }
    };

    class RuntimeContextAwareService
    {
    public:
        virtual ~RuntimeContextAwareService() = default;
        virtual void set_runtime_context(const RuntimeContext &context) = 0;
    };

}

#endif
