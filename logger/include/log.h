#ifndef __LOG_H__
#define __LOG_H__

namespace yuan::log
{
    #define DEFAULT_BUFFER_SIZE (1024 * 100)
    class Logger
    {
    public:
        enum class Level : char
        {
            trace,
            debug,
            info,
            warn,
            error,
            fatal,
        };
        
    public:
        virtual ~Logger() {}

    public:
        virtual void log(Level level, const char *fmt, ...) = 0;
    };
}

#endif
