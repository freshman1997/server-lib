#ifndef __YUAN_LOGGER_H__
#define __YUAN_LOGGER_H__

#include "registry.h"

#define LOG_GET_REGISTRY() yuan::log::LogRegistry::get_instance()

#define LOG_TRACE(...) LOG_GET_REGISTRY()->log_source(yuan::log::Level::trace, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_DEBUG(...) LOG_GET_REGISTRY()->log_source(yuan::log::Level::debug, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_INFO(...)  LOG_GET_REGISTRY()->log_source(yuan::log::Level::info, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_WARN(...)  LOG_GET_REGISTRY()->log_source(yuan::log::Level::warn, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_ERROR(...) LOG_GET_REGISTRY()->log_source(yuan::log::Level::error, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_FATAL(...) LOG_GET_REGISTRY()->log_source(yuan::log::Level::fatal, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define LOG_TRACE_TAG(tag, ...) LOG_GET_REGISTRY()->log_source(yuan::log::Level::trace, __FILE__, __LINE__, (tag), __VA_ARGS__)
#define LOG_DEBUG_TAG(tag, ...) LOG_GET_REGISTRY()->log_source(yuan::log::Level::debug, __FILE__, __LINE__, (tag), __VA_ARGS__)
#define LOG_INFO_TAG(tag, ...)  LOG_GET_REGISTRY()->log_source(yuan::log::Level::info, __FILE__, __LINE__, (tag), __VA_ARGS__)
#define LOG_WARN_TAG(tag, ...)  LOG_GET_REGISTRY()->log_source(yuan::log::Level::warn, __FILE__, __LINE__, (tag), __VA_ARGS__)
#define LOG_ERROR_TAG(tag, ...) LOG_GET_REGISTRY()->log_source(yuan::log::Level::error, __FILE__, __LINE__, (tag), __VA_ARGS__)
#define LOG_FATAL_TAG(tag, ...) LOG_GET_REGISTRY()->log_source(yuan::log::Level::fatal, __FILE__, __LINE__, (tag), __VA_ARGS__)

#define LOG_NAMED(name, level, ...)                    \
    do {                                               \
        auto _lg = LOG_GET_REGISTRY()->get_logger(name); \
        if (_lg && (level) >= LOG_GET_REGISTRY()->global_level()) \
            _lg->log_fmt(level, __VA_ARGS__);          \
    } while (0)


#endif // __YUAN_LOGGER_H__
