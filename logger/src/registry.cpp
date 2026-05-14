#include "registry.h"
#include "console_logger.h"
#include "file_logger.h"

namespace yuan::log
{

LogRegistry::LogRegistry()
{
    auto console = std::make_shared<ConsoleLogger>();
    console->set_name("default");
    default_logger_ = console;
    loggers_["default"] = console;

    auto file = std::make_shared<FileLogger>("", "", Level::trace,
                                             RotatePolicy::daily, 100 * 1024 * 1024, 7);
    file->set_name("file");
    file_logger_ = file;
    loggers_["file"] = file;
    file_enabled_.store(true, std::memory_order_relaxed);
    file_threshold_.store(Level::warn, std::memory_order_relaxed);
}

LogRegistry::~LogRegistry()
{
    flush_all();
}

void LogRegistry::register_logger(const std::string& name, std::shared_ptr<Logger> logger)
{
    std::lock_guard<std::mutex> lock(mutex_);
    loggers_[name] = logger;
    if (!default_logger_) default_logger_ = logger;
}

std::shared_ptr<Logger> LogRegistry::get_logger(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = loggers_.find(name);
    return (it != loggers_.end()) ? it->second : nullptr;
}

void LogRegistry::unregister_logger(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    loggers_.erase(name);
}

void LogRegistry::set_default(std::shared_ptr<Logger> logger)
{
    std::lock_guard<std::mutex> lock(mutex_);
    default_logger_ = logger;
}

void LogRegistry::set_global_level(Level lv)
{
    global_level_.store(lv, std::memory_order_relaxed);
}

void LogRegistry::enable_file_log(Level threshold,
                                  const std::string& log_path,
                                  const std::string& file_name,
                                  RotatePolicy policy,
                                  uint64_t max_size,
                                  int backup_count)
{
    auto file_logger = std::make_shared<FileLogger>(log_path, file_name, Level::trace,
                                                    policy, max_size, backup_count);
    file_logger->set_name("file");

    std::lock_guard<std::mutex> lock(mutex_);
    file_logger_ = file_logger;
    file_enabled_.store(true, std::memory_order_relaxed);
    file_threshold_.store(threshold, std::memory_order_relaxed);
    loggers_["file"] = file_logger;
}

void LogRegistry::enable_file_log_with_config(Level threshold, const LogConfig& cfg)
{
    auto file_logger = std::make_shared<FileLogger>(cfg);
    file_logger->set_name("file");

    std::lock_guard<std::mutex> lock(mutex_);
    file_logger_ = file_logger;
    file_enabled_.store(true, std::memory_order_relaxed);
    file_threshold_.store(threshold, std::memory_order_relaxed);
    loggers_["file"] = file_logger;
}

void LogRegistry::disable_file_log()
{
    std::lock_guard<std::mutex> lock(mutex_);
    file_enabled_.store(false, std::memory_order_relaxed);
    file_logger_.reset();
}

bool LogRegistry::is_file_enabled() const
{
    return file_enabled_.load(std::memory_order_relaxed);
}

Level LogRegistry::file_threshold() const
{
    return file_threshold_.load(std::memory_order_relaxed);
}

void LogRegistry::flush_all()
{
    std::shared_ptr<Logger> file_logger_snapshot;
    std::unordered_map<std::string, std::shared_ptr<Logger>> loggers_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loggers_snapshot = loggers_;
        file_logger_snapshot = file_logger_;
    }

    for (auto& [name, logger] : loggers_snapshot) {
        if (logger) logger->flush();
    }

    const auto file_it = loggers_snapshot.find("file");
    if (file_logger_snapshot &&
        (file_it == loggers_snapshot.end() || file_it->second != file_logger_snapshot)) {
        file_logger_snapshot->flush();
    }
}

} // namespace yuan::log
