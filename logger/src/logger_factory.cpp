#include "logger_factory.h"
#include "console_logger.h"
#include "file_logger.h"
#include <map>
#include <mutex>

namespace yuan::log
{

class LoggerFactory::InnerData
{
public:
    LogConfig cfg_;
    std::map<LoggerType, std::shared_ptr<Logger>> loggers_;
    std::map<LoggerType, LoggerCreator> creators_;
    mutable std::mutex mutex_;
};

LoggerFactory::LoggerFactory()
    : data_(std::make_unique<InnerData>())
{}

LoggerFactory::~LoggerFactory()
{
    flush_all();
}

bool LoggerFactory::init(const std::string& config_path)
{
    LogConfig cfg;
    if (!config_path.empty()) {
        cfg = LogConfig::load_from_json(config_path);
    } else {
        cfg = LogConfig::default_config();
    }
    return init_with_config(cfg);
}

bool LoggerFactory::init_with_config(const LogConfig& cfg)
{
    std::vector<std::shared_ptr<Logger>> old_loggers;
    {
        std::lock_guard<std::mutex> lock(data_->mutex_);
        for (const auto& [type, logger] : data_->loggers_) {
            if (logger) old_loggers.push_back(logger);
        }
        data_->loggers_.clear();
        data_->cfg_ = cfg;
    }

    for (const auto& logger : old_loggers) {
        logger->flush();
    }

    return true;
}

void LoggerFactory::register_creator(LoggerType type, LoggerCreator creator)
{
    std::lock_guard<std::mutex> lock(data_->mutex_);
    data_->creators_[type] = std::move(creator);
    data_->loggers_.erase(type);
}

std::shared_ptr<Logger> LoggerFactory::get_logger(LoggerType type)
{
    LogConfig cfg;
    LoggerCreator creator;
    {
        std::lock_guard<std::mutex> lock(data_->mutex_);
        auto it = data_->loggers_.find(type);
        if (it != data_->loggers_.end()) {
            return it->second;
        }
        cfg = data_->cfg_;
        auto creator_it = data_->creators_.find(type);
        if (creator_it != data_->creators_.end()) {
            creator = creator_it->second;
        }
    }

    std::shared_ptr<Logger> logger;
    switch (type) {
        case LoggerType::console_:
            logger = std::make_shared<ConsoleLogger>("console");
            logger->set_level(cfg.log_level);
            break;

        case LoggerType::file_:
            logger = std::make_shared<FileLogger>(cfg);
            break;

        case LoggerType::net_: {
            if (creator) {
                logger = creator(cfg);
            }
            break;
        }

        default: {
            if (creator) {
                logger = creator(cfg);
            }
            break;
        }
    }

    if (logger) {
        std::lock_guard<std::mutex> lock(data_->mutex_);
        auto [it, inserted] = data_->loggers_.emplace(type, logger);
        if (!inserted) {
            return it->second;
        }
    }
    return logger;
}

std::vector<std::shared_ptr<Logger>> LoggerFactory::get_all_loggers() const
{
    std::lock_guard<std::mutex> lock(data_->mutex_);
    std::vector<std::shared_ptr<Logger>> result;
    for (const auto& [type, logger] : data_->loggers_) {
        if (logger) result.push_back(logger);
    }
    return result;
}

void LoggerFactory::flush_all()
{
    std::vector<std::shared_ptr<Logger>> loggers;
    {
        std::lock_guard<std::mutex> lock(data_->mutex_);
        for (const auto& [type, logger] : data_->loggers_) {
            if (logger) loggers.push_back(logger);
        }
    }

    for (auto& logger : loggers) {
        if (logger) logger->flush();
    }
}

const LogConfig& LoggerFactory::config() const
{
    return data_->cfg_;
}

} // namespace yuan::log
