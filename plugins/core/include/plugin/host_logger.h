#ifndef __YUAN_PLUGIN_HOST_LOGGER_H__
#define __YUAN_PLUGIN_HOST_LOGGER_H__

#include <string_view>

namespace yuan::plugin
{

enum class HostLogLevel
{
    trace,
    debug,
    info,
    warn,
    error,
    fatal,
};

class HostLogger
{
public:
    virtual ~HostLogger() = default;

    virtual void log(HostLogLevel level,
                     const char *file,
                     int line,
                     const char *function,
                     std::string_view message) = 0;
};

} // namespace yuan::plugin

#endif
