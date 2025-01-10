#include "logger_factory.h"
#include "console_logger.h"

#include <map>
#include <memory>

namespace yuan::log 
{
    class LoggerFactory::InnerData
    {
    public:
        std::map<LoggerType, std::shared_ptr<Logger>> loggers_;
    };

    LoggerFactory::LoggerFactory() : data_(std::make_unique<LoggerFactory::InnerData>())
    {
    }

    LoggerFactory::~LoggerFactory()
    {}

    bool LoggerFactory::init()
    {
        return false;
    }

    std::shared_ptr<Logger> LoggerFactory::get_logger(LoggerType type)
    {
        auto it = data_->loggers_.find(type);
        if (it != data_->loggers_.end()) {
            return it->second;
        }

        switch (type) {
        case LoggerType::console_: 
        {
            auto logger = std::make_shared<ConsoleLogger>();
            data_->loggers_[type] = logger;
            return logger;
        }
        case LoggerType::file_: 
        {
            break;
        }
        case LoggerType::net_: 
        {
            break;
        }
        }
        return nullptr;
    }
}