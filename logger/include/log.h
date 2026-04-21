#ifndef __LOG_H__
#define __LOG_H__

#include "color.h"
#include <atomic>
#include <ctime>
#include <fmt/chrono.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace yuan::log
{

constexpr size_t DEFAULT_BUFFER_SIZE = 1024 * 100;

struct LogConfig;
class Formatter;

enum class Level : char
{
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    fatal = 5,
};

inline Level str_to_level(const std::string& s)
{
    if (s == "trace") return Level::trace;
    if (s == "debug") return Level::debug;
    if (s == "info") return Level::info;
    if (s == "warn") return Level::warn;
    if (s == "error") return Level::error;
    if (s == "fatal") return Level::fatal;
    return Level::info;
}

inline const char* level_to_str(Level level)
{
    switch (level) {
        case Level::trace: return "TRACE";
        case Level::debug: return "DEBUG";
        case Level::info: return "INFO";
        case Level::warn: return "WARN";
        case Level::error: return "ERROR";
        case Level::fatal: return "FATAL";
    }
    return "UNKNOWN";
}

inline std::string get_color(Level level)
{
    switch (level) {
        case Level::trace: return CLR_GREEN;
        case Level::debug: return CLR_GREEN;
        case Level::info: return CLR_SKYBLUE;
        case Level::warn: return CLR_YELLOW;
        case Level::error: return CLR_RED;
        case Level::fatal: return CLR_PURPLE_WHT;
    }
    return "";
}

struct LogItem
{
    Level level = Level::info;
    std::time_t timestamp = 0;
    uint64_t milliseconds = 0;
    std::string message;
    std::string logger_name;
    int line = 0;
    std::string source_file;
    std::string function_name;
};

class Logger
{
public:
    virtual ~Logger() = default;

    void set_level(Level lv) { level_.store(lv, std::memory_order_relaxed); }
    Level get_level() const { return level_.load(std::memory_order_relaxed); }

    void set_formatter(std::shared_ptr<Formatter> fmt);
    std::shared_ptr<Formatter> get_formatter() const { return formatter_; }

    void set_name(const std::string& name) { name_ = name; }
    const std::string& get_name() const { return name_; }

    virtual void log(Level level, const char* fmt, ...) = 0;

    void log_message(Level level, const std::string& msg)
    {
        if (level < level_.load(std::memory_order_relaxed)) return;
        log_impl(level, msg);
    }

    void log_message_source(Level level, const char* file, int line, const char* func,
                            const std::string& msg)
    {
        if (level < level_.load(std::memory_order_relaxed)) return;
        log_impl(level, msg, file, line, func);
    }

    template<typename... Args>
    void log_fmt(Level level, fmt::format_string<Args...> fmt_str, Args&&... args)
    {
        if (level < level_.load(std::memory_order_relaxed)) return;

        std::string msg;
        try {
            msg = fmt::vformat(fmt_str, fmt::make_format_args(args...));
        } catch (...) {
            msg = "[format_error]";
        }
        log_impl(level, msg);
    }

    template<typename... Args>
    void log_fmt_source(Level level, const char* file, int line, const char* func,
                        fmt::format_string<Args...> fmt_str, Args&&... args)
    {
        if (level < level_.load(std::memory_order_relaxed)) return;

        std::string msg;
        try {
            msg = fmt::vformat(fmt_str, fmt::make_format_args(args...));
        } catch (...) {
            msg = "[format_error]";
        }
        log_impl(level, msg, file, line, func);
    }

    virtual void flush() {}

protected:
    virtual void log_impl(Level level, const std::string& msg) = 0;
    virtual void log_impl(Level level, const std::string& msg,
                          const char* file, int line, const char* func)
    {
        log_impl(level, msg);
    }

    std::string format_log_item(const LogItem& item);

protected:
    std::atomic<Level> level_{Level::debug};
    std::shared_ptr<Formatter> formatter_;
    std::string name_{"default"};
    mutable std::mutex mutex_;
};

} // namespace yuan::log

#endif // __LOG_H__
