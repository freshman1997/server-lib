#ifndef __REGISTRY_H__
#define __REGISTRY_H__

#include "log.h"
#include "log_config.h"
#include "singleton/singleton.h"
#include <memory>
#include <unordered_map>

namespace yuan::log
{

/**
 * 全局日志注册中心。
 *
 * 提供统一日志入口，负责：
 * - 管理多个命名 logger
 * - 提供 `LOG_TRACE / LOG_INFO / LOG_ERROR` 等便捷宏
 * - 支持全局级别过滤
 * - 支持按阈值将日志额外分流到文件
 * - 在程序结束时统一 flush
 */
class LogRegistry : public singleton::Singleton<LogRegistry>
{
public:
    LogRegistry();
    ~LogRegistry();

    /// 注册一个命名 logger；同名时覆盖旧值。
    void register_logger(const std::string& name, std::shared_ptr<Logger> logger);

    /// 获取已注册的 logger，未找到时返回 nullptr。
    std::shared_ptr<Logger> get_logger(const std::string& name) const;

    /// 注销一个 logger。
    void unregister_logger(const std::string& name);

    /// 设置默认 logger。
    void set_default(std::shared_ptr<Logger> logger);

    /// 获取默认 logger。
    std::shared_ptr<Logger> get_default() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return default_logger_;
    }

    /// 设置全局最低日志级别，低于该级别的日志会被丢弃。
    void set_global_level(Level lv);

    /// 获取当前全局最低日志级别。
    Level global_level() const { return global_level_.load(std::memory_order_relaxed); }

    /// 启用文件分流。
    void enable_file_log(Level threshold,
                         const std::string& log_path = "./logs",
                         const std::string& file_name = "app.log",
                         RotatePolicy policy = RotatePolicy::daily,
                         uint64_t max_size = 100 * 1024 * 1024,
                         int backup_count = 7);

    /// 使用 LogConfig 启用文件分流。
    void enable_file_log_with_config(Level threshold, const LogConfig& cfg);

    /// 关闭文件分流。
    void disable_file_log();

    bool is_file_enabled() const;
    Level file_threshold() const;

    /// flush 当前已注册 logger 以及文件分流 logger。
    void flush_all();

    template<typename... Args>
    void log(Level level, fmt::format_string<Args...> fmt_str, Args&&... args)
    {
        if (level < global_level()) return;

        std::shared_ptr<Logger> default_logger;
        std::shared_ptr<Logger> file_logger;
        bool file_enabled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            default_logger = default_logger_;
            file_logger = file_logger_;
            file_enabled = file_enabled_.load(std::memory_order_relaxed);
        }

        if (!default_logger) return;

        try {
            const std::string msg = fmt::vformat(fmt_str, fmt::make_format_args(args...));
            default_logger->log_message(level, msg);
            if (file_enabled && level >= file_threshold_.load(std::memory_order_relaxed) && file_logger) {
                file_logger->log_message(level, msg);
            }
        } catch (...) {
        }
    }

    template<typename... Args>
    void log_source(Level level, const char* file, int line, const char* func,
                    fmt::format_string<Args...> fmt_str, Args&&... args)
    {
        if (level < global_level()) return;

        std::shared_ptr<Logger> default_logger;
        std::shared_ptr<Logger> file_logger;
        bool file_enabled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            default_logger = default_logger_;
            file_logger = file_logger_;
            file_enabled = file_enabled_.load(std::memory_order_relaxed);
        }

        if (!default_logger) return;

        try {
            const std::string msg = fmt::vformat(fmt_str, fmt::make_format_args(args...));
            default_logger->log_message_source(level, file, line, func, msg);
            if (file_enabled && level >= file_threshold_.load(std::memory_order_relaxed) && file_logger) {
                file_logger->log_message_source(level, file, line, func, msg);
            }
        } catch (...) {
        }
    }

private:
    std::unordered_map<std::string, std::shared_ptr<Logger>> loggers_;
    std::shared_ptr<Logger> default_logger_;
    std::shared_ptr<Logger> file_logger_;
    std::atomic<bool> file_enabled_{false};
    std::atomic<Level> file_threshold_{Level::fatal};
    std::atomic<Level> global_level_{Level::trace};
    mutable std::mutex mutex_;
};

} // namespace yuan::log

#endif // __REGISTRY_H__
