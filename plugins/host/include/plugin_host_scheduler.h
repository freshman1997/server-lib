#ifndef __YUAN_APP_PLUGIN_HOST_SCHEDULER_H__
#define __YUAN_APP_PLUGIN_HOST_SCHEDULER_H__

#include "plugin/host_scheduler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace yuan::app
{

class PluginHostScheduler : public plugin::HostScheduler
{
public:
    PluginHostScheduler();
    ~PluginHostScheduler() override;

    plugin::HostSchedulerTaskId schedule_after(std::chrono::milliseconds delay,
                                               plugin::HostSchedulerCallback callback,
                                               const std::string &name = "") override;

    plugin::HostSchedulerTaskId schedule_interval(std::chrono::milliseconds interval,
                                                  plugin::HostSchedulerCallback callback,
                                                  const std::string &name = "") override;

    bool cancel(plugin::HostSchedulerTaskId task_id) override;
    void cancel_by_prefix(const std::string &name_prefix) override;

    /// 停止调度器, 取消所有任务并等待线程退出
    void shutdown();

    /// 调度器是否仍在运行 (shutdown 后返回 false)
    bool is_running() const override;

    /// 设置空闲超时: 无任务后线程等待多久自动退出 (默认 30 秒, 0 表示永不自动退出)
    void set_idle_timeout(std::chrono::milliseconds timeout);

private:
    struct TaskInfo
    {
        plugin::HostSchedulerTaskId id = 0;
        std::string name;
        std::chrono::steady_clock::time_point next_run;
        std::chrono::milliseconds interval{}; ///< 0 表示一次性任务
        plugin::HostSchedulerCallback callback;
        bool cancelled = false;
    };

    void worker_loop();
    void insert_task(std::shared_ptr<TaskInfo> task);
    void ensure_worker_started();
    void spawn_worker();

    std::atomic<plugin::HostSchedulerTaskId> next_id_{1};
    std::atomic<bool> running_{true};
    std::atomic<bool> shutdown_requested_{false};

    std::chrono::milliseconds idle_timeout_{30000}; ///< 空闲超时

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // 线程管理: 懒启动 + 空闲回收
    std::thread worker_;
    bool worker_active_ = false; ///< 是否有线程正在运行 (受 mutex_ 保护)

    // 使用 shared_ptr 以便在 multimap 和 id_index 中共享
    std::multimap<std::chrono::steady_clock::time_point, std::shared_ptr<TaskInfo>> tasks_;
    std::unordered_map<plugin::HostSchedulerTaskId, std::shared_ptr<TaskInfo>> id_index_;
};

} // namespace yuan::app

#endif
