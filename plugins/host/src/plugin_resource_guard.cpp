#include "plugin_resource_guard.h"
#include "logger.h"

#include <algorithm>

namespace yuan::app
{

    PluginResourceGuard::~PluginResourceGuard()
    {
        cleanup_all();
    }

    uint64_t PluginResourceGuard::track(const std::string & plugin_name,
                                        plugin::PluginResourceType type,
                                        plugin::ResourceCleanupFn cleanup,
                                        const std::string & description)
    {
        if (!cleanup || plugin_name.empty()) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        auto quota_it = quotas_.find(plugin_name);
        if (quota_it != quotas_.end()) {
            const auto total = plugin_index_[plugin_name].size();
            if (total >= quota_it->second.max_total_resources) {
                LOG_WARN("resource quota exceeded for plugin '{}': total {} >= {}",
                         plugin_name, total, quota_it->second.max_total_resources);
                return 0;
            }

            auto type_limit_it = quota_it->second.max_resources_by_type.find(type);
            if (type_limit_it != quota_it->second.max_resources_by_type.end()) {
                size_t type_count = 0;
                for (auto existing_id : plugin_index_[plugin_name]) {
                    auto res_it = resources_.find(existing_id);
                    if (res_it != resources_.end() && res_it->second.type == type) {
                        ++type_count;
                    }
                }
                if (type_count >= type_limit_it->second) {
                    LOG_WARN("resource quota exceeded for plugin '{}': type {} {} >= {}",
                             plugin_name, plugin::to_string(type), type_count, type_limit_it->second);
                    return 0;
                }
            }
        }

        uint64_t id = next_id_++;

        TrackedResource resource;
        resource.id = id;
        resource.plugin_name = plugin_name;
        resource.type = type;
        resource.cleanup = std::move(cleanup);
        resource.description = description;

        resources_.emplace(id, std::move(resource));
        plugin_index_[plugin_name].push_back(id);
        return id;
    }

    bool PluginResourceGuard::untrack(uint64_t resource_id)
    {
        if (resource_id == 0) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = resources_.find(resource_id);
        if (it == resources_.end()) {
            return false;
        }

        // 从插件索引中移除
        auto &ids = plugin_index_[it->second.plugin_name];
        for (auto id_it = ids.begin(); id_it != ids.end(); ++id_it) {
            if (*id_it == resource_id) {
                ids.erase(id_it);
                break;
            }
        }
        if (ids.empty()) {
            plugin_index_.erase(it->second.plugin_name);
        }

        resources_.erase(it);
        return true;
    }

    void PluginResourceGuard::cleanup_plugin(const std::string & plugin_name)
    {
        // 先复制出需要清理的资源 (避免在回调中持有锁)
        std::vector<TrackedResource> to_cleanup;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = plugin_index_.find(plugin_name);
            if (it == plugin_index_.end()) {
                return;
            }

            for (uint64_t id : it->second) {
                auto res_it = resources_.find(id);
                if (res_it != resources_.end()) {
                    to_cleanup.push_back(std::move(res_it->second));
                    resources_.erase(res_it);
                }
            }
            plugin_index_.erase(it);
        }

        // 在锁外执行清理回调 (逆序清理, 后注册的先清理)
        for (auto rit = to_cleanup.rbegin(); rit != to_cleanup.rend(); ++rit) {
            if (rit->cleanup) {
                try
                {
                    rit->cleanup();
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR("resource cleanup error for plugin '{}': {}", plugin_name, e.what());
                }
                catch (...)
                {
                    LOG_ERROR("resource cleanup unknown error for plugin '{}'", plugin_name);
                }
            }
        }
    }

    void PluginResourceGuard::cleanup_all()
    {
        std::unordered_map<std::string, std::vector<uint64_t> > all_plugins;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            all_plugins = std::move(plugin_index_);
            plugin_index_.clear();
        }

        // 逐个插件清理
        for (auto & [
                        name,
                        ids
                    ] : all_plugins) {
            // 重新调用 cleanup_plugin 逻辑 (此时 plugin_index_ 已清空, 需直接处理)
            std::vector<TrackedResource> to_cleanup;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (uint64_t id : ids) {
                    auto it = resources_.find(id);
                    if (it != resources_.end()) {
                        to_cleanup.push_back(std::move(it->second));
                        resources_.erase(it);
                    }
                }
            }

            for (auto rit = to_cleanup.rbegin(); rit != to_cleanup.rend(); ++rit) {
                if (rit->cleanup) {
                    try
                    {
                        rit->cleanup();
                    }
                    catch (...)
                    {
                        // 吞掉异常, 确保所有资源都有机会清理
                    }
                }
            }
        }
    }

    size_t PluginResourceGuard::tracked_count(const std::string & plugin_name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = plugin_index_.find(plugin_name);
        return it == plugin_index_.end() ? 0 : it->second.size();
    }

    bool PluginResourceGuard::has_tracked_resources(const std::string & plugin_name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = plugin_index_.find(plugin_name);
        return it != plugin_index_.end() && !it->second.empty();
    }

    std::unordered_map<plugin::PluginResourceType, size_t> PluginResourceGuard::resource_snapshot(
        const std::string & plugin_name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::unordered_map<plugin::PluginResourceType, size_t> snapshot;
        auto it = plugin_index_.find(plugin_name);
        if (it == plugin_index_.end()) {
            return snapshot;
        }
        for (uint64_t id : it->second) {
            auto res_it = resources_.find(id);
            if (res_it != resources_.end()) {
                ++snapshot[res_it->second.type];
            }
        }
        return snapshot;
    }

    std::string PluginResourceGuard::leak_report(const std::string & plugin_name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string report;
        auto it = plugin_index_.find(plugin_name);
        if (it == plugin_index_.end() || it->second.empty()) {
            return report;
        }

        report += "Resource leak report for plugin '" + plugin_name + "':\n";
        std::unordered_map<plugin::PluginResourceType, size_t> type_counts;
        for (uint64_t id : it->second) {
            auto res_it = resources_.find(id);
            if (res_it != resources_.end()) {
                const auto &res = res_it->second;
                ++type_counts[res.type];
                report += "  [";
                report += plugin::to_string(res.type);
                report += "] id=";
                report += std::to_string(res.id);
                if (!res.description.empty()) {
                    report += " desc=\"";
                    report += res.description;
                    report += "\"";
                }
                report += "\n";
            }
        }

        report += "  Total: " + std::to_string(it->second.size()) + " resource(s)\n";
        for (const auto & [
                              type,
                              count
                          ] : type_counts) {
            report += "    ";
            report += plugin::to_string(type);
            report += ": ";
            report += std::to_string(count);
            report += "\n";
        }
        return report;
    }

    void PluginResourceGuard::set_quota(const std::string & plugin_name, const plugin::PluginResourceQuota & quota)
    {
        if (plugin_name.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        quotas_[plugin_name] = quota;
    }

    void PluginResourceGuard::clear_quota(const std::string & plugin_name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quotas_.erase(plugin_name);
    }

    plugin::PluginResourceQuota PluginResourceGuard::quota(const std::string & plugin_name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = quotas_.find(plugin_name);
        return it == quotas_.end() ? plugin::PluginResourceQuota{} : it->second;
    }

} // namespace yuan::app
