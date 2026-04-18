#include "logger.h"
#include "logger_factory.h"
#include <filesystem>
#include <iostream>

int main()
{
    using namespace yuan::log;

    const auto log_dir = std::filesystem::temp_directory_path() / "yuan_logger_test";
    std::cout << "cwd=" << std::filesystem::current_path().string() << "\n";
    std::cout << "log_dir=" << log_dir.string() << "\n";

    LogConfig cfg;
    cfg.log_level = Level::trace;
    cfg.log_path = log_dir.string();
    cfg.log_file_name = "utf8.log";
    cfg.rotate_policy = RotatePolicy::size_limit;
    cfg.max_file_size = 1024 * 1024;
    cfg.backup_count = 3;
    cfg.fmt_pattern = "{asctime} [{levelname}] {file}:{line} {func} - {message}";

    auto factory = LoggerFactory::get_instance();
    factory->init_with_config(cfg);

    auto console = factory->get_logger(LoggerType::console_);
    auto file = factory->get_logger(LoggerType::file_);

    if (!console || !file) {
        std::cerr << "logger init failed\n";
        return 1;
    }

    console->log_fmt_source(Level::info, __FILE__, __LINE__, __func__,
                            "中文日志验证: {}, number={}", "你好，世界", 42);
    console->log_fmt_source(Level::warn, __FILE__, __LINE__, __func__,
                            "brace payload should stay literal: {}", "{user: \"张三\"}");
    file->log_fmt_source(Level::error, __FILE__, __LINE__, __func__,
                         "文件输出验证: 路径={}, 状态={}", "D:/tmp/测试", "成功");

    LOG_INFO("registry 宏验证: {}", "默认 logger 中文正常");
    LOG_ERROR("registry 宏写文件验证: {}", "这一行应进入默认文件分流");

    factory->flush_all();
    std::cout << "test_logger completed\n";
    return 0;
}
