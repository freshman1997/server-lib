#ifndef __YUAN_PLUGIN_PLUGIN_SANDBOX_DELEGATE_H__
#define __YUAN_PLUGIN_PLUGIN_SANDBOX_DELEGATE_H__

#include "plugin/plugin_manifest.h"
#include "plugin/plugin_state.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace yuan::plugin
{

    struct SandboxConfig
    {
        std::string sandbox_id;
        PluginRunMode run_mode = PluginRunMode::multi_process;
        std::chrono::milliseconds startup_timeout{ 5000 };
        std::chrono::milliseconds heartbeat_interval{ 1000 };
        std::size_t max_memory_mb = 256;
        uint32_t max_faults = 3;
    };

    class PluginSandboxDelegate
    {
    public:
        virtual ~PluginSandboxDelegate() = default;

        virtual bool start(const SandboxConfig &config) = 0;
        virtual bool stop() = 0;

        virtual bool is_alive() const = 0;

        virtual bool send_init(const PluginManifest &manifest,
                               const std::string &config_json) = 0;

        virtual bool send_enable() = 0;
        virtual bool send_disable() = 0;
        virtual bool send_config_changed(const std::string &config_json) = 0;
        virtual bool send_health_check() = 0;

        using ResponseHandler = std::function<void(const std::string &response_json)>;
        virtual bool send_request(const std::string &method,
                                  const std::string &payload_json,
                                  ResponseHandler handler) = 0;

        virtual SandboxConfig config() const = 0;
        virtual PluginState sandbox_state() const = 0;
    };

    class PluginSandboxHost
    {
    public:
        virtual ~PluginSandboxHost() = default;

        virtual std::unique_ptr<PluginSandboxDelegate> create_sandbox(const SandboxConfig &config) = 0;

        virtual std::vector<std::string> active_sandboxes() const = 0;
        virtual bool terminate_sandbox(const std::string &sandbox_id) = 0;
    };

} // namespace yuan::plugin

#endif
