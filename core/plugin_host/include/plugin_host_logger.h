#ifndef __YUAN_APP_PLUGIN_HOST_LOGGER_H__
#define __YUAN_APP_PLUGIN_HOST_LOGGER_H__

#include "plugin/host_logger.h"

namespace yuan::app
{

class PluginHostLogger : public plugin::HostLogger
{
public:
    void log(plugin::HostLogLevel level,
             const char *file,
             int line,
             const char *function,
             std::string_view message) override;
};

} // namespace yuan::app

#endif
