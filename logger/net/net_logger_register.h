#ifndef __NET_LOGGER_REGISTER_H__
#define __NET_LOGGER_REGISTER_H__

/**
 * NetLogger 注册辅助。
 *
 * 在链接了 `NetLogger` 扩展库的程序初始化阶段调用一次：
 *
 * ```cpp
 * #include "net_logger_register.h"
 *
 * int main() {
 *     yuan::log::register_net_logger();
 * }
 * ```
 *
 * 之后可通过 `LoggerFactory::get_logger(LoggerType::net_)` 获取网络 logger。
 */

#include "logger_factory.h"
#include "net_logger.h"

namespace yuan::log
{

inline void register_net_logger()
{
    auto factory = LoggerFactory::get_instance();
    factory->register_creator(
        LoggerType::net_,
        [](const LogConfig& cfg) -> std::shared_ptr<Logger> {
            return std::make_shared<NetLogger>(cfg);
        });
}

} // namespace yuan::log

#endif // __NET_LOGGER_REGISTER_H__
