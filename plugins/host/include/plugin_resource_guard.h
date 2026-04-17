#ifndef __YUAN_APP_PLUGIN_RESOURCE_GUARD_H__
#define __YUAN_APP_PLUGIN_RESOURCE_GUARD_H__

#include "plugin/host_resource_guard.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::app
{

    /// 具体实现: 基于 mutex 保护的资源追踪
    class PluginResourceGuard : public plugin::HostResourceGuard
    {
    public:
        PluginResourceGuard() = default;
        ~PluginResourceGuard() override;

        uint64_t track(const std::string &plugin_name,
                       plugin::PluginResourceType type,
                       plugin::ResourceCleanupFn cleanup,
                       const std::string &description = "") override;

        bool untrack(uint64_t resource_id) override;

        void cleanup_plugin(const std::string &plugin_name) override;

        void cleanup_all() override;

        size_t tracked_count(const std::string &plugin_name) const override;

        bool has_tracked_resources(const std::string &plugin_name) const override;

        std::unordered_map<plugin::PluginResourceType, size_t> resource_snapshot(
            const std::string &plugin_name) const override;

        std::string leak_report(const std::string &plugin_name) const override;

    private:
        struct TrackedResource
        {
            uint64_t id;
            std::string plugin_name;
            plugin::PluginResourceType type;
            plugin::ResourceCleanupFn cleanup;
            std::string description;
        };

        mutable std::mutex mutex_;
        uint64_t next_id_ = 1;
        std::unordered_map<uint64_t, TrackedResource> resources_;              // id -> resource
        std::unordered_map<std::string, std::vector<uint64_t> > plugin_index_; // plugin_name -> [ids]
    };

} // namespace yuan::app

#endif
