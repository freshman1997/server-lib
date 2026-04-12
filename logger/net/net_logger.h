#ifndef __NET_LOGGER_H__
#define __NET_LOGGER_H__

#include "log.h"
#include "log_config.h"

namespace yuan::log
{

/**
 * 网络日志输出。
 *
 * 当前实现具备以下特性：
 * - 对外暴露 `connect / disconnect / is_connected`
 * - 内部通过不透明实现指针隔离 core 网络依赖
 * - 支持连接失败后的自动重连
 * - 支持待发送队列上限与溢出策略
 * - 最终失败时可回退到本地 `stderr`
 */
class NetLogger : public Logger
{
public:
    explicit NetLogger(const LogConfig& cfg);
    NetLogger(const std::string& server_ip, int server_port);
    ~NetLogger() override;

public:
    void log(Level level, const char* fmt, ...) override;
    void flush() override;

    /// 尝试连接远端日志服务。
    bool connect();

    /// 主动断开连接。
    void disconnect();

    /// 当前是否已连接。
    bool is_connected() const;

protected:
    void log_impl(Level level, const std::string& msg) override;
    void log_impl(Level level, const std::string& msg, const char* file, int line, const char* func) override;

private:
    LogConfig config_;
    void* connection_impl_ = nullptr;
    mutable std::mutex conn_mutex_;
};

} // namespace yuan::log

#endif // __NET_LOGGER_H__
