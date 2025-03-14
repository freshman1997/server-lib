#ifndef __FILE_LOGGER_H__
#define __FILE_LOGGER_H__
#include "log.h"

namespace yuan::log 
{
    class FileLogger : public Logger
    {
    public:
        FileLogger();
        ~FileLogger();
        
    public:
        virtual void log(Level level, const char *fmt, ...);
    };
}

#endif // __FILE_LOGGER_H__
