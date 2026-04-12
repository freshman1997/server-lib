#include "logger_factory.h"
#include "console_logger.h"
#include "file_logger.h"
#include <map>

namespace yuan::log
{

class LoggerFactory::InnerData
{
public:
    LogConfig cfg_;
    std::map<LoggerType, std::shared_ptr<Logger>> loggers_;
    std::map<LoggerType, LoggerCreator> creators_;
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
    if (!config_path.empty()) {
        data_->cfg_ = LogConfig::load_from_json(config_path);
    } else {
        data_->cfg_ = LogConfig::default_config();
    }
    return init_with_config(data_->cfg_);
}

bool LoggerFactory::init_with_config(const LogConfig& cfg)
{
    data_->cfg_ = cfg;
    return true;
}

void LoggerFactory::register_creator(LoggerType type, LoggerCreator creator)
{
    data_->creators_[type] = std::move(creator);
}

std::shared_ptr<Logger> LoggerFactory::get_logger(LoggerType type)
{
    auto it = data_->loggers_.find(type);
    if (it != data_->loggers_.end()) {
        return it->second;
    }

    std::shared_ptr<Logger> logger;
    const auto& cfg = data_->cfg_;

    switch (type) {
        case LoggerType::console_:
            logger = std::make_shared<ConsoleLogger>("console");
            logger->set_level(cfg.log_level);
            break;

        case LoggerType::file_:
            logger = std::make_shared<FileLogger>(cfg);
            break;

        case LoggerType::net_: {
            auto creator_it = data_->creators_.find(type);
            if (creator_it != data_->creators_.end() && creator_it->second) {
                logger = creator_it->second(cfg);
            }
            break;
        }

        default: {
            auto creator_it = data_->creators_.find(type);
            if (creator_it != data_->creators_.end() && creator_it->second) {
                logger = creator_it->second(cfg);
            }
            break;
        }
    }

    if (logger) {
        data_->loggers_[type] = logger;
    }
    return logger;
}

std::vector<std::shared_ptr<Logger>> LoggerFactory::get_all_loggers() const
{
    std::vector<std::shared_ptr<Logger>> result;
    for (const auto& [type, logger] : data_->loggers_) {
        if (logger) result.push_back(logger);
    }
    return result;
}

void LoggerFactory::flush_all()
{
    for (auto& [type, logger] : data_->loggers_) {
        if (logger) logger->flush();
    }
}

const LogConfig& LoggerFactory::config() const
{
    return data_->cfg_;
}

} // namespace yuan::log
