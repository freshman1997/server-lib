#ifndef __LOG_CONSOLE_LOGGER_H__
#define __LOG_CONSOLE_LOGGER_H__
#include "log.h"

namespace yuan::log 
{
    class ConsoleLogger : public Logger
    {
    public:
        ConsoleLogger() = default;
        ~ConsoleLogger();
        
    public:
        virtual void log(Level level, const char *fmt, ...);
    };
}

#endif // __LOG_CONSOLE_LOGGER_H__