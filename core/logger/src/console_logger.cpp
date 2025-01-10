#include "console_logger.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "color.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

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
        char time_buffer[20];
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

        color.append("[%s] %s\n");

        if (written >= 0 && written < size) {
            printf(color.c_str(), time_buffer, buf->peek());
        } else {
            printf("[%s] Log message too long or error occurred.\n", time_buffer);
        }

        buf->reset();
    }
}