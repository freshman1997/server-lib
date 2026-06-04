#ifndef __YREDIS_FORMATTER_H__
#define __YREDIS_FORMATTER_H__

#include <memory>
#include <string>
#include "redis_value.h"

namespace yredis
{
    enum class FormatStyle
    {
        raw,
        pretty,
        json,
    };

    std::string format_value(const std::shared_ptr<yuan::redis::RedisValue> &val, FormatStyle style = FormatStyle::pretty);

    std::string type_label(char resp_type);

    const char *ansi_reset();
    const char *ansi_bold();
    const char *ansi_red();
    const char *ansi_green();
    const char *ansi_yellow();
    const char *ansi_cyan();
    const char *ansi_gray();
    const char *ansi_magenta();
}

#endif