#include "plugin_host_logger.h"

#include "registry.h"

#include <string>

namespace yuan::app
{

namespace
{

yuan::log::Level to_log_level(plugin::HostLogLevel level)
{
    switch (level)
    {
    case plugin::HostLogLevel::trace:
        return yuan::log::Level::trace;
    case plugin::HostLogLevel::debug:
        return yuan::log::Level::debug;
    case plugin::HostLogLevel::info:
        return yuan::log::Level::info;
    case plugin::HostLogLevel::warn:
        return yuan::log::Level::warn;
    case plugin::HostLogLevel::error:
        return yuan::log::Level::error;
    case plugin::HostLogLevel::fatal:
        return yuan::log::Level::fatal;
    }
    return yuan::log::Level::info;
}

} // namespace

void PluginHostLogger::log(plugin::HostLogLevel level,
                           const char *file,
                           int line,
                           const char *function,
                           std::string_view message)
{
    yuan::log::LogRegistry::get_instance()->log_source(
        to_log_level(level),
        file,
        line,
        function,
        "{}",
        std::string(message));
}

} // namespace yuan::app
