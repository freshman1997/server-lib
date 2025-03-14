#ifndef __NET_LOGGER_H__
#define __NET_LOGGER_H__
#include "log.h"

namespace yuan::log 
{
    class NetLogger : public Logger
    {
    public:
        virtual void log(Level level, const char *fmt, ...);
    };
}

#endif // __NET_LOGGER_H__
