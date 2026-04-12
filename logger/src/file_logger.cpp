#include "file_logger.h"
#include "base/time.h"
#include "formatter.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace yuan::log
{

namespace
{

static std::string get_process_name()
{
    char buf[512] = {0};
#ifdef _WIN32
    const DWORD len = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) return "app";
#else
    const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "app";
    buf[len] = '\0';
#endif

    std::string path(buf);
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) path = path.substr(slash + 1);

    const size_t dot = path.rfind('.');
    if (dot != std::string::npos) path = path.substr(0, dot);

    return path.empty() ? "app" : path;
}

static inline LogItem make_log_item(Level level, const std::string& msg, const std::string& name,
                                    const char* file = nullptr, int line = 0, const char* func = nullptr)
{
    LogItem item;
    item.level = level;
    item.message = msg;
    item.logger_name = name;
    item.line = line;

    const auto now_ms = yuan::base::time::system_now_ms();
    item.timestamp = static_cast<std::time_t>(now_ms / 1000ULL);
    item.milliseconds = now_ms % 1000ULL;

    if (file) item.source_file = file;
    if (func) item.function_name = func;
    return item;
}

static std::string vformat_message(const char* fmt, va_list args)
{
    char stack_buf[4096];
    va_list args_copy;
    va_copy(args_copy, args);
    const int written = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, args_copy);
    va_end(args_copy);

    if (written < 0) return {};
    if (static_cast<size_t>(written) < sizeof(stack_buf)) {
        return std::string(stack_buf, static_cast<size_t>(written));
    }

    std::vector<char> heap_buf(static_cast<size_t>(written) + 1, '\0');
    va_list args_retry;
    va_copy(args_retry, args);
    std::vsnprintf(heap_buf.data(), heap_buf.size(), fmt, args_retry);
    va_end(args_retry);
    return std::string(heap_buf.data(), static_cast<size_t>(written));
}

} // namespace

FileLogger::FileLogger(const LogConfig& cfg)
    : config_(cfg), last_open_date_(current_date_str())
{
    set_level(cfg.log_level);
    formatter_ = std::make_shared<Formatter>(cfg.fmt_pattern, cfg.fmt_datefmt);

    static std::string proc_name;
    static std::once_flag flag;
    std::call_once(flag, [] { proc_name = get_process_name(); });

    if (config_.log_path == "./logs") {
        config_.log_path = "logs/" + proc_name;
    }
    if (config_.log_file_name == "app.log") {
        config_.log_file_name = proc_name + ".log";
    }

    initialized_ = open_new_file();
}

FileLogger::FileLogger(const std::string& log_path,
                       const std::string& file_name,
                       Level level,
                       RotatePolicy rotate_policy,
                       uint64_t max_file_size,
                       int backup_count)
{
    config_.log_path = log_path.empty() ? ("logs/" + get_process_name()) : log_path;
    config_.log_file_name = file_name.empty() ? (get_process_name() + ".log") : file_name;
    config_.log_level = level;
    config_.rotate_policy = rotate_policy;
    config_.max_file_size = max_file_size;
    config_.backup_count = backup_count;
    set_level(level);
    formatter_ = std::make_shared<Formatter>();
    last_open_date_ = current_date_str();
    initialized_ = open_new_file();
}

FileLogger::~FileLogger()
{
    flush();
    if (ofs_.is_open()) ofs_.close();
}

void FileLogger::log(Level level, const char* fmt, ...)
{
    if (level < level_.load(std::memory_order_relaxed)) return;

    va_list args;
    va_start(args, fmt);
    const std::string msg = vformat_message(fmt, args);
    va_end(args);

    log_impl(level, msg);
}

void FileLogger::log_impl(Level level, const std::string& msg)
{
    LogItem item = make_log_item(level, msg, name_);
    std::string formatted;
    try {
        formatted = format_log_item(item);
    } catch (...) {
        formatted = "[format_error] " + msg;
    }

    std::lock_guard<std::mutex> lock(file_mutex_);
    check_and_rotate();

    if (!open_new_file()) return;
    ofs_ << formatted << '\n';
    ofs_.flush();
    current_file_size_.store(current_file_size_.load(std::memory_order_relaxed) + formatted.size() + 1,
                             std::memory_order_relaxed);
    ofs_.close();
}

void FileLogger::log_impl(Level level, const std::string& msg,
                          const char* file, int line, const char* func)
{
    LogItem item = make_log_item(level, msg, name_, file, line, func);
    std::string formatted;
    try {
        formatted = format_log_item(item);
    } catch (...) {
        formatted = "[format_error] " + msg;
    }

    std::lock_guard<std::mutex> lock(file_mutex_);
    check_and_rotate();

    if (!open_new_file()) return;
    ofs_ << formatted << '\n';
    ofs_.flush();
    current_file_size_.store(current_file_size_.load(std::memory_order_relaxed) + formatted.size() + 1,
                             std::memory_order_relaxed);
    ofs_.close();
}

void FileLogger::flush()
{
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (ofs_.is_open()) ofs_.flush();
}

bool FileLogger::need_time_rotate() const
{
    if (config_.rotate_policy == RotatePolicy::daily) {
        return current_date_str() != last_open_date_;
    }
    if (config_.rotate_policy == RotatePolicy::hourly) {
        return current_datetime_hour_str() != last_open_date_;
    }
    return false;
}

bool FileLogger::need_size_rotate() const
{
    if (config_.rotate_policy != RotatePolicy::size_limit) return false;
    return current_file_size_.load(std::memory_order_relaxed) >= config_.max_file_size;
}

void FileLogger::check_and_rotate()
{
    if (need_time_rotate() || need_size_rotate()) {
        do_rotate();
    }
}

std::string FileLogger::current_date_str()
{
    const auto tm_buf = yuan::base::time::localtime(yuan::base::time::system_now_seconds());
    char buf[16] = {0};
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm_buf);
    return std::string(buf);
}

std::string FileLogger::current_datetime_hour_str()
{
    const auto tm_buf = yuan::base::time::localtime(yuan::base::time::system_now_seconds());
    char buf[16] = {0};
    std::strftime(buf, sizeof(buf), "%Y%m%d%H", &tm_buf);
    return std::string(buf);
}

bool FileLogger::open_new_file()
{
    if (ofs_.is_open()) ofs_.close();

    try {
        fs::create_directories(config_.log_path);
    } catch (...) {
    }

    const auto full_path = config_.log_path + "/" + config_.log_file_name;
    ofs_.open(full_path, std::ios::out | std::ios::app | std::ios::binary);
    if (!ofs_.is_open()) return false;

    ofs_.seekp(0, std::ios::end);
    current_file_size_.store(static_cast<uint64_t>(ofs_.tellp()), std::memory_order_relaxed);
    last_open_date_ = (config_.rotate_policy == RotatePolicy::hourly) ? current_datetime_hour_str()
                                                                      : current_date_str();
    return true;
}

void FileLogger::do_rotate()
{
    if (ofs_.is_open()) ofs_.close();

    const auto base_path = config_.log_path + "/" + config_.log_file_name;
    if (!fs::exists(base_path)) {
        current_file_size_.store(0, std::memory_order_relaxed);
        last_open_date_ = (config_.rotate_policy == RotatePolicy::hourly) ? current_datetime_hour_str()
                                                                          : current_date_str();
        return;
    }

    std::string time_part;
    switch (config_.rotate_policy) {
        case RotatePolicy::hourly:
            time_part = current_datetime_hour_str();
            break;
        case RotatePolicy::size_limit: {
            const auto tm_buf = yuan::base::time::localtime(yuan::base::time::system_now_seconds());
            char buf[32] = {0};
            std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm_buf);
            time_part = buf;
            break;
        }
        case RotatePolicy::daily:
        case RotatePolicy::none:
        default:
            time_part = current_date_str();
            break;
    }

    int max_idx = 0;
    for (int i = 1; i <= config_.backup_count + 10; ++i) {
        const auto candidate = base_path + "." + time_part + "." + std::to_string(i);
        if (fs::exists(candidate)) {
            max_idx = i;
        }
    }

    try {
        fs::rename(base_path, base_path + "." + time_part + "." + std::to_string(max_idx + 1));
    } catch (...) {
    }

    current_file_size_.store(0, std::memory_order_relaxed);
    clean_old_backups();
    last_open_date_ = (config_.rotate_policy == RotatePolicy::hourly) ? current_datetime_hour_str()
                                                                      : current_date_str();
}

void FileLogger::clean_old_backups()
{
    if (config_.backup_count <= 0) return;

    const auto base_path = config_.log_path + "/" + config_.log_file_name;

    struct BackupInfo
    {
        std::string path;
        fs::file_time_type mtime;
    };

    std::vector<BackupInfo> backups;
    try {
        if (!fs::exists(config_.log_path)) return;

        for (const auto& entry : fs::directory_iterator(config_.log_path)) {
            const auto path = entry.path().string();
            if (path.rfind(base_path + ".", 0) == 0) {
                backups.push_back({path, fs::last_write_time(entry)});
            }
        }
    } catch (...) {
        return;
    }

    if (static_cast<int>(backups.size()) <= config_.backup_count) return;

    std::sort(backups.begin(), backups.end(),
              [](const BackupInfo& a, const BackupInfo& b) { return a.mtime < b.mtime; });

    const int to_remove = static_cast<int>(backups.size()) - config_.backup_count;
    for (int i = 0; i < to_remove; ++i) {
        try {
            fs::remove(backups[static_cast<size_t>(i)].path);
        } catch (...) {
        }
    }
}

} // namespace yuan::log
