#ifndef __LOG_CONSOLE_LOGGER_H__
#define __LOG_CONSOLE_LOGGER_H__
#include "log.h"
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <ctime>
#include <string>

namespace yuan::log
{
    class ConsoleLogger : public Logger
    {
    public:
        ConsoleLogger() = default;
        ~ConsoleLogger();

    public:
        virtual void log(Level level, const char *fmt, ...) override;

    protected:
        virtual void log_impl(Level level, const std::string& msg) override;

    private:
        std::string get_color(Level level);
        std::string get_level_str(Level level);
    };
}

#endif // __LOG_CONSOLE_LOGGER_H__