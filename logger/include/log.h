#ifndef __LOG_H__
#define __LOG_H__
#include <fmt/core.h>
#include <string>

namespace yuan::log
{
    #define DEFAULT_BUFFER_SIZE (1024 * 100)
    class Logger
    {
    public:
        enum class Level : char
        {
            trace,
            debug,
            info,
            warn,
            error,
            fatal,
        };

    public:
        virtual ~Logger() {}

    public:
        virtual void log(Level level, const char *fmt, ...) = 0;

        template<typename... Args>
        void log_fmt(Level level, fmt::format_string<Args...> fmt_str, Args&&... args)
        {
            std::string formatted_msg = fmt::vformat(fmt_str, fmt::make_format_args(args...));
            log_impl(level, formatted_msg);
        }

    protected:
        virtual void log_impl(Level level, const std::string& msg) = 0;
    };
}

#endif
