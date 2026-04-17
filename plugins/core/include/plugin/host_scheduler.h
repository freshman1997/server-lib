#ifndef __YUAN_PLUGIN_HOST_SCHEDULER_H__
#define __YUAN_PLUGIN_HOST_SCHEDULER_H__

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace yuan::plugin
{

using HostSchedulerTaskId = std::uint64_t;

using HostSchedulerCallback = std::function<void()>;

class HostScheduler
{
public:
    virtual ~HostScheduler() = default;

    /// 延迟执行一次, 返回任务 ID (0 表示失败)
    virtual HostSchedulerTaskId schedule_after(std::chrono::milliseconds delay,
                                               HostSchedulerCallback callback,
                                               const std::string &name = "") = 0;

    /// 固定间隔重复执行, 返回任务 ID (0 表示失败)
    virtual HostSchedulerTaskId schedule_interval(std::chrono::milliseconds interval,
                                                  HostSchedulerCallback callback,
                                                  const std::string &name = "") = 0;

    /// 取消定时任务
    virtual bool cancel(HostSchedulerTaskId task_id) = 0;

    /// 取消所有由指定插件注册的任务 (按 name 前缀匹配)
    virtual void cancel_by_prefix(const std::string &name_prefix) = 0;

    /// 调度器是否仍在运行 (shutdown 后返回 false)
    virtual bool is_running() const = 0;
};

} // namespace yuan::plugin

#endif
