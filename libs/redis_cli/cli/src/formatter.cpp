#include "formatter.h"
#include "value/array_value.h"
#include "value/error_value.h"
#include "value/float_value.h"
#include "value/int_value.h"
#include "value/map_value.h"
#include "value/null_value.h"
#include "value/status_value.h"
#include "value/string_value.h"
#include "internal/def.h"

#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>

namespace yredis
{
    static bool is_terminal()
    {
        const char *no_color = std::getenv("NO_COLOR");
        if (no_color && no_color[0] != '\0') return false;
#ifdef _WIN32
        return _isatty(_fileno(stdout)) != 0;
#else
        const char *term = std::getenv("TERM");
        if (!term || term[0] == '\0') return false;
        return isatty(STDOUT_FILENO) != 0;
#endif
    }

    const char *ansi_reset() { return is_terminal() ? "\033[0m" : ""; }
    const char *ansi_bold() { return is_terminal() ? "\033[1m" : ""; }
    const char *ansi_red() { return is_terminal() ? "\033[31m" : ""; }
    const char *ansi_green() { return is_terminal() ? "\033[32m" : ""; }
    const char *ansi_yellow() { return is_terminal() ? "\033[33m" : ""; }
    const char *ansi_cyan() { return is_terminal() ? "\033[36m" : ""; }
    const char *ansi_gray() { return is_terminal() ? "\033[90m" : ""; }
    const char *ansi_magenta() { return is_terminal() ? "\033[35m" : ""; }

    std::string type_label(char resp_type)
    {
        switch (resp_type)
        {
        case yuan::redis::resp_status: return "status";
        case yuan::redis::resp_error: return "error";
        case yuan::redis::resp_string: return "string";
        case yuan::redis::resp_int: return "int";
        case yuan::redis::resp_null: return "null";
        case yuan::redis::resp_float: return "float";
        case yuan::redis::resp_bool: return "bool";
        case yuan::redis::resp_array: return "array";
        case yuan::redis::resp_map: return "map";
        case yuan::redis::resp_set: return "set";
        default: return "unknown";
        }
    }

    static constexpr int max_format_depth = 32;

    static std::string format_pretty(const std::shared_ptr<yuan::redis::RedisValue> &val, int depth)
    {
        if (!val) return std::string(ansi_gray()) + "(nil)\n" + ansi_reset();

        if (depth > max_format_depth) {
            return std::string(ansi_gray()) + "(too deeply nested)\n" + ansi_reset();
        }

        char t = val->get_type();

        if (t == yuan::redis::resp_null)
        {
            return std::string(ansi_gray()) + "(nil)\n" + ansi_reset();
        }

        if (t == yuan::redis::resp_error)
        {
            return std::string(ansi_red()) + "(error) " + val->to_string() + "\n" + ansi_reset();
        }

        if (t == yuan::redis::resp_status)
        {
            const auto &raw = val->get_raw_str();
            if (raw.empty() || raw == "OK")
            {
                return std::string(ansi_green()) + "OK\n" + ansi_reset();
            }
            return std::string(ansi_green()) + raw + "\n" + ansi_reset();
        }

        if (t == yuan::redis::resp_int)
        {
            auto iv = val->as<yuan::redis::IntValue>();
            return std::string(ansi_cyan()) + "(integer) " + std::to_string(iv ? iv->get_value() : 0) + "\n" + ansi_reset();
        }

        if (t == yuan::redis::resp_float)
        {
            return std::string(ansi_cyan()) + "(float) " + val->to_string() + "\n" + ansi_reset();
        }

        if (t == yuan::redis::resp_string)
        {
            std::string s = val->to_string();
            if (s.empty())
            {
                return std::string(ansi_gray()) + "(empty string)\n" + ansi_reset();
            }

            bool has_binary = false;
            bool has_newline = false;
            for (unsigned char c : s)
            {
                if (c == 0 || (c < 32 && c != '\n' && c != '\r' && c != '\t') || c > 126)
                {
                    has_binary = true;
                    break;
                }
                if (c == '\n') has_newline = true;
            }

            if (has_binary)
            {
                std::ostringstream oss;
                for (unsigned char c : s)
                {
                    if (c >= 32 && c <= 126 && c != '\\')
                    {
                        oss << c;
                    }
                    else
                    {
                        char hex[5];
                        std::snprintf(hex, sizeof(hex), "\\x%02x", c);
                        oss << hex;
                    }
                }
                return "\"" + oss.str() + "\"\n";
            }

            if (has_newline)
            {
                return s + "\n";
            }

            return "\"" + s + "\"\n";
        }

        if (t == yuan::redis::resp_array)
        {
            auto arr = val->as<yuan::redis::ArrayValue>();
            if (!arr) return val->to_string();

            const auto &items = arr->get_values();
            if (items.empty())
            {
                return std::string(ansi_gray()) + "(empty array)\n" + ansi_reset();
            }

            std::string prefix(depth * 2, ' ');
            std::ostringstream oss;
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                oss << prefix << std::setw(3) << (i + 1) << ") " << format_pretty(items[i], depth + 1);
            }
            return oss.str();
        }

        if (t == yuan::redis::resp_map)
        {
            auto mv = val->as<yuan::redis::MapValue>();
            if (!mv) return val->to_string();

            const auto &map = mv->get_map_value();
            if (map.empty())
            {
                return std::string(ansi_gray()) + "(empty map)\n" + ansi_reset();
            }

            std::string prefix(depth * 2, ' ');
            std::ostringstream oss;
            std::size_t idx = 1;
            for (const auto &[k, v] : map)
            {
                oss << prefix << std::setw(3) << idx << ") " << ansi_yellow() << k << ansi_reset()
                    << " -> " << format_pretty(v, depth + 1);
                ++idx;
            }
            return oss.str();
        }

        return val->to_string();
    }

    static std::string format_raw(const std::shared_ptr<yuan::redis::RedisValue> &val)
    {
        if (!val) return "(nil)";
        char t = val->get_type();
        std::string prefix;
        switch (t)
        {
        case yuan::redis::resp_status:
        {
            const auto &raw = val->get_raw_str();
            return "+" + (raw.empty() ? val->to_string() : raw) + "\n";
        }
        case yuan::redis::resp_error: prefix = "-"; break;
        case yuan::redis::resp_string: prefix = "$"; break;
        case yuan::redis::resp_int: prefix = ":"; break;
        case yuan::redis::resp_null: prefix = "_"; break;
        case yuan::redis::resp_float: prefix = ","; break;
        case yuan::redis::resp_array: prefix = "*"; break;
        case yuan::redis::resp_map: prefix = "%"; break;
        default: prefix = "?"; break;
        }
        return prefix + val->to_string() + "\n";
    }

    std::string format_value(const std::shared_ptr<yuan::redis::RedisValue> &val, FormatStyle style)
    {
        switch (style)
        {
        case FormatStyle::pretty: return format_pretty(val, 0);
        case FormatStyle::raw: return format_raw(val);
        default: return format_pretty(val, 0);
        }
    }
}