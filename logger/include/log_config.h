#ifndef __LOG_CONFIG_H__
#define __LOG_CONFIG_H__

#include "log.h"
#include <cstdint>
#include <string>

namespace yuan::log
{

/// 文件轮转策略。
enum class RotatePolicy : int
{
    none = 0,
    daily = 1,
    hourly = 2,
    size_limit = 3,
};

struct LogConfig
{
    // 基础配置
    std::string version = "1.0";
    std::string log_path = "./logs";
    std::string log_file_name = "app.log";
    Level log_level = Level::debug;
    bool async_mode = false;

    // 轮转配置
    RotatePolicy rotate_policy = RotatePolicy::none;
    uint64_t max_file_size = 100 * 1024 * 1024;
    int backup_count = 7;
    std::string encoding = "utf-8";

    // 网络配置
    std::string net_server_ip = "127.0.0.1";
    int net_server_port = 9999;
    bool net_auto_reconnect = true;
    int net_connect_timeout_ms = 1000;
    int net_reconnect_delay_ms = 250;
    int net_max_retries = -1;           // -1 表示无限重试
    int net_max_pending_messages = 10000;
    bool net_drop_oldest_on_overflow = true;

    // 格式配置
    std::string fmt_pattern = "{asctime} - [{func}] {file}:{line} - {levelname} - {message}";
    std::string fmt_datefmt = "%Y-%m-%d %H:%M:%S";

    /// 从 JSON 配置文件加载。
    static LogConfig load_from_json(const std::string& json_path);

    /// 从 JSON 字符串加载。
    static LogConfig parse_from_json_string(const std::string& json_str);

    /// 返回默认配置。
    static LogConfig default_config();
};

} // namespace yuan::log

#endif // __LOG_CONFIG_H__
