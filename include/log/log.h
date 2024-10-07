#ifndef __LOG_H__
#define __LOG_H__
#include "../buffer/buffer.h"

namespace mlog
{
    #define DEFAULT_BUFFER_SIZE (1 << 24)
    class Logger
    {
    public:
        enum class Level : char
        {
            debug,
            info,
            warn,
            error,
            fatal,
        };
    public:
        Logger();
        virtual ~Logger();

    public:
        virtual void log(Level level, const char *fmt, ...) = 0;

    protected:
        Buffer buff_;
    };
}

#endif
