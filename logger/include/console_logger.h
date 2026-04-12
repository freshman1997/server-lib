#ifndef __LOG_CONSOLE_LOGGER_H__
#define __LOG_CONSOLE_LOGGER_H__

#include "log.h"
#include <string>

namespace yuan::log
{

class ConsoleLogger : public Logger
{
public:
    explicit ConsoleLogger(const std::string& name = "console");
    ~ConsoleLogger() override;

    void log(Level level, const char *fmt, ...) override;
    void flush() override;

protected:
    void log_impl(Level level, const std::string& msg) override;
    void log_impl(Level level, const std::string& msg, const char* file, int line, const char* func) override;
};

} // namespace yuan::log

#endif // __LOG_CONSOLE_LOGGER_H__
