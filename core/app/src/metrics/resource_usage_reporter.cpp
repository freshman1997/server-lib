#include "metrics/resource_usage_reporter.h"

#include "base/system/resource_usage.h"
#include "logger.h"
#include "timer/timer_manager.h"

#include <atomic>
#include <mutex>
#include <sstream>

namespace yuan::app::metrics
{
    namespace
    {
        std::atomic_bool process_reporter_running{false};
        std::mutex process_reporter_mutex;
        ResourceUsageReporter process_reporter;

        std::string summarize_network_interfaces(const yuan::base::system::NetworkUsage &network)
        {
            std::ostringstream stream;
            std::size_t written = 0;
            for (const auto &interface : network.interfaces) {
                if (interface.loopback) {
                    continue;
                }
                if (written > 0) {
                    stream << "; ";
                }
                stream << interface.name
                       << "(up=" << (interface.up ? 1 : 0)
                       << ",rx=" << interface.receive_bytes
                       << ",tx=" << interface.transmit_bytes
                       << ")";
                ++written;
                if (written >= 8) {
                    break;
                }
            }
            return written == 0 ? std::string("none") : stream.str();
        }
    }

    ResourceUsageReporter::~ResourceUsageReporter()
    {
        stop();
    }

    void ResourceUsageReporter::start(timer::TimerManager &timer_manager, ResourceUsageReportOptions options)
    {
        stop();
        if (!options.enabled || options.interval_ms == 0) {
            return;
        }

        bool expected = false;
        if (!process_reporter_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        owns_process_reporter_ = true;

        auto previous_cpu = yuan::base::system::current_process_cpu_times();
        auto previous_network = yuan::base::system::current_network_usage();
        const auto name = std::move(options.name);

        timer_ = timer_manager.every(options.interval_ms, options.interval_ms,
            [previous_cpu, previous_network, name]() mutable {
                const auto usage = yuan::base::system::current_resource_usage();
                const auto cpu_percent = yuan::base::system::process_cpu_percent(previous_cpu, usage.cpu);
                const auto rx_bps = yuan::base::system::network_receive_bytes_per_second(previous_network, usage.network);
                const auto tx_bps = yuan::base::system::network_transmit_bytes_per_second(previous_network, usage.network);

                if (name.empty()) {
                    LOG_INFO(
                        "resource usage cpu={:.2f}% rss={:.2f}MB vmem={:.2f}MB mem_used={:.2f}% disk_used={:.2f}% net_rx={:.2f}B/s net_tx={:.2f}B/s net_ifaces={}",
                        cpu_percent,
                        usage.memory.resident_mb(),
                        usage.memory.virtual_mb(),
                        usage.memory.system_used_percent(),
                        usage.disk.used_percent(),
                        rx_bps,
                        tx_bps,
                        summarize_network_interfaces(usage.network));
                } else {
                    LOG_INFO(
                        "resource usage name={} cpu={:.2f}% rss={:.2f}MB vmem={:.2f}MB mem_used={:.2f}% disk_used={:.2f}% net_rx={:.2f}B/s net_tx={:.2f}B/s net_ifaces={}",
                        name,
                        cpu_percent,
                        usage.memory.resident_mb(),
                        usage.memory.virtual_mb(),
                        usage.memory.system_used_percent(),
                        usage.disk.used_percent(),
                        rx_bps,
                        tx_bps,
                        summarize_network_interfaces(usage.network));
                }

                previous_cpu = usage.cpu;
                previous_network = usage.network;
            });
        if (!timer_) {
            owns_process_reporter_ = false;
            process_reporter_running.store(false, std::memory_order_release);
        }
    }

    void ResourceUsageReporter::stop()
    {
        timer_.cancel();
        timer_.reset();
        if (owns_process_reporter_) {
            owns_process_reporter_ = false;
            process_reporter_running.store(false, std::memory_order_release);
        }
    }

    bool ResourceUsageReporter::running() const noexcept
    {
        return static_cast<bool>(timer_);
    }

    void start_process_resource_usage_reporter(timer::TimerManager &timer_manager, ResourceUsageReportOptions options)
    {
        std::lock_guard<std::mutex> lock(process_reporter_mutex);
        if (!process_reporter.running()) {
            process_reporter.start(timer_manager, std::move(options));
        }
    }

    void stop_process_resource_usage_reporter()
    {
        std::lock_guard<std::mutex> lock(process_reporter_mutex);
        process_reporter.stop();
    }
}
