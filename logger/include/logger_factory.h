#ifndef __MLOG_LOGGER_FACTORY_H__
#define __MLOG_LOGGER_FACTORY_H__

#include "log.h"
#include "log_config.h"
#include "singleton/singleton.h"
#include <functional>
#include <memory>
#include <vector>

namespace yuan::log
{

enum class LoggerType : int
{
    console_ = 1 << 0,
    file_ = 1 << 1,
    net_ = 1 << 2,
};

inline LoggerType operator|(LoggerType a, LoggerType b)
{
    return static_cast<LoggerType>(static_cast<int>(a) | static_cast<int>(b));
}

using LoggerCreator = std::function<std::shared_ptr<Logger>(const LogConfig&)>;

/**
 * logger 工厂。
 *
 * 设计目标是将基础日志能力和扩展 logger 解耦：
 * - `Logger` 基础库只包含 `ConsoleLogger` / `FileLogger`
 * - `NetLogger` 这类扩展通过 `register_creator()` 注入
 * - 调用方按需获取 logger，工厂内部负责缓存复用
 */
class LoggerFactory : public singleton::Singleton<LoggerFactory>
{
public:
    LoggerFactory();
    ~LoggerFactory();

    /// 通过 JSON 配置初始化。
    bool init(const std::string& config_path = "");

    /// 直接使用已有配置初始化。
    bool init_with_config(const LogConfig& cfg);

    /// 按类型获取 logger；首次请求时创建，后续复用缓存。
    std::shared_ptr<Logger> get_logger(LoggerType type);

    /// 返回当前已创建的 logger 列表。
    std::vector<std::shared_ptr<Logger>> get_all_loggers() const;

    /// flush 所有已创建的 logger。
    void flush_all();

    /// 获取当前生效配置。
    const LogConfig& config() const;

    /// 注册扩展 logger 的创建器。
    void register_creator(LoggerType type, LoggerCreator creator);

private:
    class InnerData;
    std::unique_ptr<InnerData> data_;
};

} // namespace yuan::log

#endif // __MLOG_LOGGER_FACTORY_H__
