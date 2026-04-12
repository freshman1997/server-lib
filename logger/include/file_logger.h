#ifndef __FILE_LOGGER_H__
#define __FILE_LOGGER_H__

#include "log.h"
#include "log_config.h"
#include <atomic>
#include <ctime>
#include <fstream>
#include <mutex>

namespace yuan::log
{

/**
 * 文件日志输出，支持按天、按小时、按大小轮转，并自动清理旧备份。
 */
class FileLogger : public Logger
{
public:
    explicit FileLogger(const LogConfig& cfg);

    FileLogger(const std::string& log_path,
               const std::string& file_name,
               Level level = Level::debug,
               RotatePolicy rotate_policy = RotatePolicy::daily,
               uint64_t max_file_size = 100 * 1024 * 1024,
               int backup_count = 7);

    ~FileLogger() override;

public:
    void log(Level level, const char* fmt, ...) override;
    void flush() override;

protected:
    void log_impl(Level level, const std::string& msg) override;
    void log_impl(Level level, const std::string& msg, const char* file, int line, const char* func) override;

private:
    void check_and_rotate();
    bool open_new_file();
    bool need_time_rotate() const;
    bool need_size_rotate() const;
    void do_rotate();
    void clean_old_backups();

    static std::string current_date_str();
    static std::string current_datetime_hour_str();

private:
    LogConfig config_;
    std::ofstream ofs_;
    std::atomic<uint64_t> current_file_size_{0};
    std::string last_open_date_;
    mutable std::mutex file_mutex_;
    bool initialized_ = false;
};

} // namespace yuan::log

#endif // __FILE_LOGGER_H__
