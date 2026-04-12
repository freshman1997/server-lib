#include "formatter.h"
#include <cstdio>
#include <ctime>
#include <fmt/core.h>

namespace yuan::log
{

namespace
{

static std::string extract_filename(const std::string& path)
{
#ifdef _WIN32
    auto pos = path.find_last_of("\\/");
#else
    auto pos = path.rfind('/');
#endif
    return (pos != std::string::npos) ? path.substr(pos + 1) : path;
}

static bool is_reasonable_function_name(const std::string& s)
{
    if (s.empty()) return false;
    for (unsigned char c : s) {
        const bool is_alpha_num = (c >= 'a' && c <= 'z') ||
                                  (c >= 'A' && c <= 'Z') ||
                                  (c >= '0' && c <= '9');
        const bool is_allowed_punct = c == '_' || c == ':' || c == '~' ||
                                      c == '<' || c == '>' || c == '=' ||
                                      c == '!' || c == '+' || c == '-' ||
                                      c == '*' || c == '/' || c == '&' ||
                                      c == '|' || c == '^' || c == '%' ||
                                      c == '.' || c == ',' || c == '(' ||
                                      c == ')' || c == '[' || c == ']' ||
                                      c == ' ';
        if (!is_alpha_num && !is_allowed_punct) {
            return false;
        }
    }
    return true;
}

} // namespace

Formatter::Formatter(const std::string& pattern, const std::string& datefmt)
    : pattern_(pattern), datefmt_(datefmt)
{}

std::string Formatter::format_time(std::time_t t, const std::string& fmt)
{
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    char buf[128] = {0};
    std::strftime(buf, sizeof(buf), fmt.c_str(), &tm_buf);
    return std::string(buf);
}

std::string Formatter::format(const LogItem& item) const
{
    std::string time_str = format_time(item.timestamp, datefmt_);
    char ms_buf[5] = {0};
    std::snprintf(ms_buf, sizeof(ms_buf), ".%03u",
                  static_cast<unsigned>(item.milliseconds % 1000));
    time_str += ms_buf;

    const std::string level_name = level_to_str(item.level);
    const std::string file_name = item.source_file.empty() ? "-" : extract_filename(item.source_file);
    const long long timestamp = static_cast<long long>(item.timestamp);
    const std::string func_name =
        (!item.function_name.empty() && is_reasonable_function_name(item.function_name))
            ? item.function_name
            : "-";
    auto arg_asctime = fmt::arg("asctime", time_str);
    auto arg_levelname = fmt::arg("levelname", level_name);
    auto arg_message = fmt::arg("message", item.message);
    auto arg_name = fmt::arg("name", item.logger_name);
    auto arg_timestamp = fmt::arg("timestamp", timestamp);
    auto arg_file = fmt::arg("file", file_name);
    auto arg_line = fmt::arg("line", item.line);
    auto arg_func = fmt::arg("func", func_name);

    try {
        return fmt::vformat(
            pattern_,
            fmt::make_format_args(arg_asctime, arg_levelname, arg_message, arg_name,
                                  arg_timestamp, arg_file, arg_line, arg_func));
    } catch (...) {
        return "[FORMAT_ERROR] " + item.message;
    }
}

} // namespace yuan::log
