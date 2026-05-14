#include "file_logger.h"
#include "formatter.h"
#include "log_config.h"
#include "logger_factory.h"

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

    LogConfig rotate_cfg;
    rotate_cfg.log_path = (root / "rotate").string();
    rotate_cfg.log_file_name = "size.log";
    rotate_cfg.log_level = Level::trace;
    rotate_cfg.rotate_policy = RotatePolicy::size_limit;
    rotate_cfg.max_file_size = 32;
    rotate_cfg.backup_count = 3;
    rotate_cfg.fmt_pattern = "{message}";

    FileLogger rotate_logger(rotate_cfg);
    rotate_logger.log_fmt(Level::info, "{}", "111111111111111111111111");
    rotate_logger.log_fmt(Level::info, "{}", "222222222222222222222222");
    rotate_logger.flush();

    const auto active_rotate_content = read_file(root / "rotate" / "size.log");
    require(active_rotate_content.find("222222222222222222222222") != std::string::npos,
            "active log file should contain latest record after explicit flush");

    int backup_files = 0;
    for (const auto& entry : fs::directory_iterator(root / "rotate")) {
        const auto name = entry.path().filename().string();
        if (name.rfind("size.log.", 0) == 0) ++backup_files;
    }
    require(backup_files > 0, "size rotation should create a backup before oversize write");

    LogConfig factory_cfg1;
    factory_cfg1.log_path = (root / "factory1").string();
    factory_cfg1.log_file_name = "factory.log";
    factory_cfg1.log_level = Level::trace;
    factory_cfg1.fmt_pattern = "{message}";

    LogConfig factory_cfg2 = factory_cfg1;
    factory_cfg2.log_path = (root / "factory2").string();

    auto factory = LoggerFactory::get_instance();
    factory->init_with_config(factory_cfg1);
    auto old_file_logger = factory->get_logger(LoggerType::file_);
    factory->init_with_config(factory_cfg2);
    auto new_file_logger = factory->get_logger(LoggerType::file_);
    require(old_file_logger != new_file_logger, "factory reinit should drop cached logger instances");
    new_file_logger->log_fmt(Level::info, "{}", "factory reinit target");
    new_file_logger->flush();
    require(fs::exists(root / "factory2" / "factory.log"), "factory reinit should use new config path");

    fs::remove_all(root, ec);
    std::cout << "logger_auto test passed\n";
    return 0;
}
