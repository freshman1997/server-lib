#ifndef __MLOG_LOGGER_FACTORY_H__
#define __MLOG_LOGGER_FACTORY_H__
#include "../singleton/singleton.h"
#include <memory>

namespace yuan::log 
{
    class Logger;

    enum LoggerType
    {
        console_ = 1,
        file_ = 2,
        net_ = 4,
    };

    class LoggerFactory : public singleton::Singleton<LoggerFactory>
    {
    public:
        bool init();

        std::shared_ptr<Logger> get_logger(int loggerTypes);

        
    };
}

#endif