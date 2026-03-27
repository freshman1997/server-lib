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
        // TODO: 可从配置文件加载日志配置
        return true;
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
            // TODO: 实现文件日志器
            break;
        }
        case LoggerType::net_: 
        {
            // TODO: 实现网络日志器
            break;
        }
        }
        return nullptr;
    }
}