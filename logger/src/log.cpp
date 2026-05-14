#include "log.h"
#include "formatter.h"

namespace yuan::log
{

void Logger::set_formatter(std::shared_ptr<Formatter> fmt)
{
    std::lock_guard<std::mutex> lock(mutex_);
    formatter_ = std::move(fmt);
}

std::string Logger::format_log_item(const LogItem& item)
{
    std::shared_ptr<Formatter> formatter;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        formatter = formatter_;
    }

    if (formatter) {
        return formatter->format(item);
    }

    try {
        return fmt::format("[{}] {}.{:03d} - {}", level_to_str(item.level),
                           static_cast<long long>(item.timestamp),
                           static_cast<int>(item.milliseconds % 1000),
                           item.message);
    } catch (...) {
        return "[format_error] " + item.message;
    }
}

} // namespace yuan::log
