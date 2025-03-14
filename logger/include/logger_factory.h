#ifndef __MLOG_LOGGER_FACTORY_H__
#define __MLOG_LOGGER_FACTORY_H__
#include "singleton/singleton.h"
#include <memory>

namespace yuan::log 
{
    class Logger;

    enum class LoggerType
    {
        console_ = 1,
        file_ = 2,
        net_ = 4,
    };

    class LoggerFactory : public singleton::Singleton<LoggerFactory>
    {
    public:
        LoggerFactory();
        ~LoggerFactory();
        
    public:
        bool init();

        std::shared_ptr<Logger> get_logger(LoggerType type);

    private:
        class InnerData;
        std::unique_ptr<InnerData> data_;
    };
}

#endif