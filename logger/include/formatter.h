#ifndef __LOG_FORMATTER_H__
#define __LOG_FORMATTER_H__

#include "log.h"
#include <ctime>
#include <memory>
#include <string>

namespace yuan::log
{

/**
 * 日志格式化器，基于 {fmt} 的命名参数能力。
 *
 * 支持的占位符：
 *   {asctime}   - 按 datefmt 格式化后的本地时间，带毫秒
 *   {levelname} - 日志级别名称
 *   {message}   - 日志消息
 *   {name}      - logger 名称
 *   {timestamp} - Unix 时间戳（秒）
 *   {file}      - 源文件名
 *   {line}      - 源码行号
 *   {func}      - 函数名
 *
 * 示例：
 *   "{asctime} - [{func}] {file}:{line} - {levelname} - {message}"
 */
class Formatter
{
public:
    explicit Formatter(const std::string& pattern = "{asctime} - {levelname} - {file}:{line} [{func}] - {message}",
                       const std::string& datefmt  = "%Y-%m-%d %H:%M:%S");

    /// 根据 pattern 和 LogItem 生成最终字符串。
    std::string format(const LogItem& item) const;

    void set_pattern(const std::string& p) { pattern_ = p; }
    void set_datefmt(const std::string& d) { datefmt_ = d; }

    const std::string& get_pattern() const { return pattern_; }
    const std::string& get_datefmt() const { return datefmt_; }

private:
    /// 将 time_t 按 datefmt 格式化为本地时间字符串。
    static std::string format_time(std::time_t t, const std::string& fmt);

private:
    std::string pattern_;
    std::string datefmt_;
};

} // namespace yuan::log

#endif // __LOG_FORMATTER_H__
