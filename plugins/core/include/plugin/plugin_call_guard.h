#ifndef __YUAN_PLUGIN_PLUGIN_CALL_GUARD_H__
#define __YUAN_PLUGIN_PLUGIN_CALL_GUARD_H__

#include "plugin/plugin_state.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace yuan::plugin
{

    struct FaultEvent
    {
        std::string plugin_name;
        std::string call_site;
        std::string error_message;
        std::chrono::steady_clock::time_point timestamp;
    };

    using FaultEventHandler = std::function<void(const FaultEvent &)>;

    class PluginCallGuard
    {
    public:
        struct Config
        {
            uint32_t fault_threshold = 3;
            uint32_t quarantine_threshold = 5;
            std::chrono::steady_clock::duration fault_window = std::chrono::seconds(60);
        };

        PluginCallGuard()
            : config_(Config{})
        {
        }

        explicit PluginCallGuard(Config config)
            : config_(std::move(config))
        {
        }

        template <typename Fn, typename... Args>
        auto guarded_call(const std::string &plugin_name,
                          PluginState plugin_state,
                          const std::string &call_site,
                          Fn &&fn,
                          Args &&... args) -> decltype(auto)
        {
            if (!accepts_callbacks(plugin_state)) {
                return decltype(fn(std::forward<Args>(args)...)) {};
            }

            try
            {
                return std::forward<Fn>(fn)(std::forward<Args>(args)...);
            }
            catch (const std::exception &ex)
            {
                record_fault(plugin_name, call_site, ex.what());
                throw;
            }
            catch (...)
            {
                record_fault(plugin_name, call_site, "unknown exception");
                throw;
            }
        }

        template <typename Fn>
        auto guarded_call_void(const std::string &plugin_name,
                               PluginState plugin_state,
                               const std::string &call_site,
                               Fn &&fn) -> bool
        {
            if (!accepts_callbacks(plugin_state)) {
                return false;
            }

            try
            {
                std::forward<Fn>(fn)();
                return true;
            }
            catch (const std::exception &ex)
            {
                record_fault(plugin_name, call_site, ex.what());
                return false;
            }
            catch (...)
            {
                record_fault(plugin_name, call_site, "unknown exception");
                return false;
            }
        }

        uint32_t fault_count(const std::string &plugin_name) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = fault_counts_.find(plugin_name);
            return it != fault_counts_.end() ? it->second.count : 0;
        }

        PluginState suggested_state(const std::string &plugin_name) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = fault_counts_.find(plugin_name);
            if (it == fault_counts_.end()) {
                return PluginState::active;
            }
            if (it->second.count >= config_.quarantine_threshold) {
                return PluginState::quarantined;
            }
            if (it->second.count >= config_.fault_threshold) {
                return PluginState::faulted;
            }
            if (it->second.count > 0) {
                return PluginState::degraded;
            }
            return PluginState::active;
        }

        void reset_faults(const std::string &plugin_name)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fault_counts_.erase(plugin_name);
        }

        void set_fault_handler(FaultEventHandler handler)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fault_handler_ = std::move(handler);
        }

        void report_fault(const std::string &plugin_name,
                          const std::string &call_site,
                          const std::string &error_message)
        {
            record_fault(plugin_name, call_site, error_message);
        }

    private:
        void record_fault(const std::string &plugin_name,
                          const std::string &call_site,
                          const std::string &error_message)
        {
            FaultEvent event;
            event.plugin_name = plugin_name;
            event.call_site = call_site;
            event.error_message = error_message;
            event.timestamp = std::chrono::steady_clock::now();

            FaultEventHandler handler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto &entry = fault_counts_[plugin_name];
                entry.count++;
                entry.last_fault_time = event.timestamp;
                handler = fault_handler_;
            }

            if (handler) {
                handler(event);
            }
        }

        Config config_;
        mutable std::mutex mutex_;

        struct FaultEntry
        {
            uint32_t count = 0;
            std::chrono::steady_clock::time_point last_fault_time;
        };

        std::unordered_map<std::string, FaultEntry> fault_counts_;
        FaultEventHandler fault_handler_;
    };

} // namespace yuan::plugin

#endif
