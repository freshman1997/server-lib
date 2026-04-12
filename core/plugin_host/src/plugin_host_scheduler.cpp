#include "plugin_host_scheduler.h"

#include "logger.h"

namespace yuan::app
{

PluginHostScheduler::PluginHostScheduler()
{
    // 不再在构造时启动线程 — 懒启动, 首次 schedule 时才创建
}

PluginHostScheduler::~PluginHostScheduler()
{
    shutdown();
}

void PluginHostScheduler::shutdown()
{
    if (!running_.exchange(false)) {
        return;
    }
    shutdown_requested_ = true;
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }

    // 清理所有未执行的任务 (此时 worker 已退出, 无需加锁)
    tasks_.clear();
    id_index_.clear();
}

plugin::HostSchedulerTaskId PluginHostScheduler::schedule_after(
    std::chrono::milliseconds delay,
    plugin::HostSchedulerCallback callback,
    const std::string &name)
{
    if (!callback || !running_.load()) {
        return 0;
    }

    auto task = std::make_shared<TaskInfo>();
    task->id = next_id_.fetch_add(1);
    task->name = name;
    task->next_run = std::chrono::steady_clock::now() + delay;
    task->interval = std::chrono::milliseconds::zero(); // 一次性
    task->callback = std::move(callback);

    const auto id = task->id;
    insert_task(std::move(task));
    return id;
}

plugin::HostSchedulerTaskId PluginHostScheduler::schedule_interval(
    std::chrono::milliseconds interval,
    plugin::HostSchedulerCallback callback,
    const std::string &name)
{
    if (!callback || interval.count() <= 0 || !running_.load()) {
        return 0;
    }

    auto task = std::make_shared<TaskInfo>();
    task->id = next_id_.fetch_add(1);
    task->name = name;
    task->next_run = std::chrono::steady_clock::now() + interval;
    task->interval = interval;
    task->callback = std::move(callback);

    const auto id = task->id;
    insert_task(std::move(task));
    return id;
}

bool PluginHostScheduler::cancel(plugin::HostSchedulerTaskId task_id)
{
    if (task_id == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = id_index_.find(task_id);
    if (it == id_index_.end()) {
        return false;
    }

    it->second->cancelled = true;
    id_index_.erase(it);
    return true;
}

void PluginHostScheduler::cancel_by_prefix(const std::string &name_prefix)
{
    if (name_prefix.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = id_index_.begin(); it != id_index_.end();) {
        if (it->second->name.rfind(name_prefix, 0) == 0) {
            it->second->cancelled = true;
            it = id_index_.erase(it);
        } else {
            ++it;
        }
    }
}

bool PluginHostScheduler::is_running() const
{
    return running_.load();
}

void PluginHostScheduler::set_idle_timeout(std::chrono::milliseconds timeout)
{
    idle_timeout_ = timeout;
}

void PluginHostScheduler::insert_task(std::shared_ptr<TaskInfo> task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id_index_[task->id] = task;
        tasks_.emplace(task->next_run, std::move(task));
    }
    ensure_worker_started();
}

void PluginHostScheduler::ensure_worker_started()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!worker_active_ && running_.load() && !shutdown_requested_.load()) {
        spawn_worker();
    } else {
        cv_.notify_all();
    }
}

void PluginHostScheduler::spawn_worker()
{
    // 调用者必须持有 mutex_
    if (worker_.joinable()) {
        worker_.join(); // 回收之前的线程 (如果有的话)
    }
    worker_active_ = true;
    worker_ = std::thread(&PluginHostScheduler::worker_loop, this);
}

void PluginHostScheduler::worker_loop()
{
    LOG_DEBUG("scheduler worker thread started");

    while (running_.load() && !shutdown_requested_.load()) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (tasks_.empty()) {
            // ---- 空闲等待: 等待新任务或空闲超时 ----
            auto wait_result = cv_.wait_for(lock, idle_timeout_, [this] {
                return !tasks_.empty() || !running_.load() || shutdown_requested_.load();
            });

            if (!running_.load() || shutdown_requested_.load()) {
                break;
            }

            if (!wait_result && tasks_.empty()) {
                // 空闲超时且仍无任务 → 退出线程 (懒回收)
                LOG_DEBUG("scheduler worker idle timeout, exiting thread");
                worker_active_ = false;
                return; // 线程退出
            }

            // 有新任务进来, 继续循环处理
            if (tasks_.empty()) {
                continue;
            }
        }

        auto now = std::chrono::steady_clock::now();

        // If a new earlier task arrives while waiting, wake up and re-evaluate
        // tasks_.begin() instead of sleeping until the old deadline.
        if (tasks_.begin()->first > now) {
            auto next_time = tasks_.begin()->first;
            cv_.wait_until(lock, next_time, [this, next_time] {
                return !running_.load() ||
                       shutdown_requested_.load() ||
                       tasks_.empty() ||
                       tasks_.begin()->first < next_time;
            });

            if (!running_.load() || shutdown_requested_.load()) {
                break;
            }

            if (tasks_.empty() || tasks_.begin()->first < next_time) {
                continue;
            }

            now = std::chrono::steady_clock::now();
        }

        // 收集到期的任务
        std::vector<std::shared_ptr<TaskInfo>> ready_tasks;
        auto it = tasks_.begin();
        while (it != tasks_.end() && it->first <= now) {
            if (!it->second->cancelled) {
                ready_tasks.push_back(it->second);
            }
            it = tasks_.erase(it);
        }

        // 执行到期任务
        for (auto &task : ready_tasks) {
            if (task->cancelled) {
                id_index_.erase(task->id);
                continue;
            }

            if (task->interval == std::chrono::milliseconds::zero()) {
                // 一次性任务, 从索引中移除后执行
                id_index_.erase(task->id);
                lock.unlock();
                if (task->callback) {
                    task->callback();
                }
                lock.lock();
            } else {
                // 重复任务: 先执行, 再重新调度
                auto callback = task->callback;
                lock.unlock();
                if (callback) {
                    callback();
                }
                lock.lock();

                if (!task->cancelled && running_.load() && !shutdown_requested_.load()) {
                    task->next_run = std::chrono::steady_clock::now() + task->interval;
                    tasks_.emplace(task->next_run, task);
                } else {
                    id_index_.erase(task->id);
                }
            }

            if (!running_.load() || shutdown_requested_.load()) {
                break;
            }
        }
    }

    // ---- 优雅退出: 执行所有已到期但未执行的一次性任务 ----
    LOG_DEBUG("scheduler worker thread shutting down");
    {
        std::unique_lock<std::mutex> lock(mutex_);

        // 取消所有重复任务
        for (auto &[id, task] : id_index_) {
            if (task->interval != std::chrono::milliseconds::zero()) {
                task->cancelled = true;
            }
        }

        // 收集已到期的一次性任务
        auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<TaskInfo>> pending_oneshot;
        auto it = tasks_.begin();
        while (it != tasks_.end()) {
            if (it->first <= now && !it->second->cancelled &&
                it->second->interval == std::chrono::milliseconds::zero()) {
                pending_oneshot.push_back(it->second);
            }
            it = tasks_.erase(it);
        }

        for (auto &task : pending_oneshot) {
            if (task->callback && !task->cancelled) {
                lock.unlock();
                task->callback();
                lock.lock();
            }
        }

        tasks_.clear();
        id_index_.clear();
        worker_active_ = false;
    }
}

} // namespace yuan::app
