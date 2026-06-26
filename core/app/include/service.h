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

        virtual void stop() = 0;

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
