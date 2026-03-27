#include "console_logger.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "color.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fmt/core.h>

namespace yuan::log 
{
    ConsoleLogger::~ConsoleLogger()
    {}

    void ConsoleLogger::log(Level level, const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);

        // 获取当前时间
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_buffer[50];
        strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);

        auto buf = buffer::BufferedPool::get_instance()->allocate(1024);
        // 格式化日志消息
        int size = buf->writable_size();
        int written = vsnprintf(buf->peek_for(), size, fmt, args);
        va_end(args);

        std::string color;
        if (level == Level::debug) {
            color = CLR_GREEN;
        } else if (level == Level::info) {
            color = CLR_BLUE;
        } else if (level == Level::warn) {
            color = CLR_YELLOW;
        } else if (level == Level::error) {
            color = CLR_RED;
        } else if (level == Level::fatal) {
            color = CLR_PURPLE;
        }

        if (written >= 0 && written < size) {
            fmt::print("[INFO {} ({})] {}\n{}", time_buffer, now, buf->peek(), CLR_CLR);
        } else {
            fmt::println("[ERROR {}({})] Log message too long or error occurred.\n", time_buffer, now);
        }

        buffer::BufferedPool::get_instance()->free(buf);
    }

    void ConsoleLogger::log_impl(Level level, const std::string& msg)
    {
        // 获取当前时间
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_buffer[50];
        strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);

        fmt::print("{}[{} {} ({})] {}{}\n", get_color(level), get_level_str(level), time_buffer, now, msg, CLR_CLR);
    }

    std::string ConsoleLogger::get_color(Level level)
    {
        switch (level) {
            case Level::trace: return CLR_GREEN;
            case Level::debug: return CLR_GREEN;
            case Level::info:  return CLR_BLUE;
            case Level::warn:  return CLR_YELLOW;
            case Level::error: return CLR_RED;
            case Level::fatal: return CLR_PURPLE;
        }
        return "";
    }

    std::string ConsoleLogger::get_level_str(Level level)
    {
        switch (level) {
            case Level::trace: return "TRACE";
            case Level::debug: return "DEBUG";
            case Level::info:  return "INFO";
            case Level::warn:  return "WARN";
            case Level::error: return "ERROR";
            case Level::fatal: return "FATAL";
        }
        return "";
    }
}
