#include "file_logger.h"
#include "formatter.h"
#include "log_config.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{

void require(bool cond, const std::string& message)
{
    if (!cond) {
        throw std::runtime_error(message);
    }
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
    namespace fs = std::filesystem;
    using namespace yuan::log;

    const auto root = fs::temp_directory_path() / "yuan_logger_auto_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const std::string json = R"({
        "log_level": "debug",
        "net_auto_reconnect": true,
        "net_connect_timeout_ms": 1200,
        "net_reconnect_delay_ms": 300,
        "net_max_retries": 4,
        "net_max_pending_messages": 256,
        "net_drop_oldest_on_overflow": false,
        "fmt": "{levelname}:{message}",
        "fmt_datefmt": "%Y-%m-%d",
        "rotate": {
            "policy": "size",
            "max_file_size": 2048,
            "backup_count": 5,
            "encoding": "utf-8"
        }
    })";
    auto cfg = LogConfig::parse_from_json_string(json);
    require(cfg.rotate_policy == RotatePolicy::size_limit, "rotate policy parse failed");
    require(cfg.max_file_size == 2048, "max_file_size parse failed");
    require(cfg.backup_count == 5, "backup_count parse failed");
    require(cfg.encoding == "utf-8", "encoding parse failed");
    require(cfg.net_auto_reconnect, "net_auto_reconnect parse failed");
    require(cfg.net_connect_timeout_ms == 1200, "net_connect_timeout_ms parse failed");
    require(cfg.net_reconnect_delay_ms == 300, "net_reconnect_delay_ms parse failed");
    require(cfg.net_max_retries == 4, "net_max_retries parse failed");
    require(cfg.net_max_pending_messages == 256, "net_max_pending_messages parse failed");
    require(!cfg.net_drop_oldest_on_overflow, "net_drop_oldest_on_overflow parse failed");

    Formatter formatter("{levelname}:{message}", "%Y-%m-%d %H:%M:%S");
    LogItem item;
    item.level = Level::warn;
    item.timestamp = std::time(nullptr);
    item.milliseconds = 7;
    item.message = R"({user: "张三"})";
    auto rendered = formatter.format(item);
    require(rendered.find(R"({user: "张三"})") != std::string::npos, "formatter should keep braces literal");
    require(rendered.find("{{") == std::string::npos, "formatter should not double braces");

    Formatter source_formatter("{file}:{line} [{func}] {message}", "%Y-%m-%d %H:%M:%S");
    item.source_file = R"(D:\code\src\vs\webserver\core\core\src\net\connection\tcp_connection.cpp)";
    item.line = 70;
    item.function_name = "~TcpConnection";
    item.message = "connection closed";
    rendered = source_formatter.format(item);
    require(rendered == "tcp_connection.cpp:70 [~TcpConnection] connection closed",
            "formatter should keep source file/function references alive");

    LogConfig file_cfg;
    file_cfg.log_path = root.string();
    file_cfg.log_file_name = "logger_utf8.log";
    file_cfg.log_level = Level::trace;
    file_cfg.rotate_policy = RotatePolicy::none;
    file_cfg.fmt_pattern = "{levelname}:{message}";

    FileLogger logger(file_cfg);
    logger.set_name("auto");
    logger.log_fmt_source(Level::info, __FILE__, __LINE__, __func__, "中文输出 {}", "测试");
    logger.log_fmt_source(Level::warn, __FILE__, __LINE__, __func__, "花括号 {}", R"({value: 1})");
    logger.flush();

    const auto log_path = root / "logger_utf8.log";
    for (int i = 0; i < 20 && !fs::exists(log_path); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    require(fs::exists(log_path), "log file was not created");

    const auto content = read_file(log_path);
    require(content.find("中文输出 测试") != std::string::npos, "utf-8 content not written");
    require(content.find("{value: 1}") != std::string::npos, "brace payload not written correctly");

    fs::remove_all(root, ec);
    std::cout << "logger_auto test passed\n";
    return 0;
}
