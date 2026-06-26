#ifndef __BASE_SYSTEM_RESOURCE_USAGE_H__
#define __BASE_SYSTEM_RESOURCE_USAGE_H__

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::base::system
{
    struct CpuTimes
    {
        uint64_t user_us = 0;
        uint64_t system_us = 0;
        uint64_t timestamp_us = 0;

        [[nodiscard]] uint64_t total_us() const { return user_us + system_us; }
        [[nodiscard]] bool valid() const { return timestamp_us != 0; }
    };

    struct MemoryUsage
    {
        uint64_t resident_bytes = 0;
        uint64_t virtual_bytes = 0;
        uint64_t system_total_bytes = 0;
        uint64_t system_available_bytes = 0;

        [[nodiscard]] double resident_mb() const;
        [[nodiscard]] double virtual_mb() const;
        [[nodiscard]] double system_used_percent() const;
    };

    struct NetworkInterfaceUsage
    {
        std::string name;
        bool up = false;
        bool loopback = false;
        uint64_t receive_bytes = 0;
        uint64_t receive_packets = 0;
        uint64_t receive_errors = 0;
        uint64_t transmit_bytes = 0;
        uint64_t transmit_packets = 0;
        uint64_t transmit_errors = 0;
    };

    struct NetworkUsage
    {
        uint64_t timestamp_us = 0;
        std::vector<NetworkInterfaceUsage> interfaces;

        [[nodiscard]] bool valid() const { return timestamp_us != 0; }
        [[nodiscard]] const NetworkInterfaceUsage *find_interface(const std::string &name) const;
        [[nodiscard]] uint64_t total_receive_bytes() const;
        [[nodiscard]] uint64_t total_transmit_bytes() const;
    };

    struct DiskUsage
    {
        std::string path;
        uint64_t capacity_bytes = 0;
        uint64_t free_bytes = 0;
        uint64_t available_bytes = 0;

        [[nodiscard]] uint64_t used_bytes() const;
        [[nodiscard]] double used_percent() const;
    };

    struct ResourceUsage
    {
        CpuTimes cpu;
        MemoryUsage memory;
        NetworkUsage network;
        DiskUsage disk;
    };

    [[nodiscard]] CpuTimes current_process_cpu_times();
    [[nodiscard]] double process_cpu_percent(const CpuTimes &previous, const CpuTimes &current);
    [[nodiscard]] MemoryUsage current_memory_usage();
    [[nodiscard]] NetworkUsage current_network_usage();
    [[nodiscard]] double network_receive_bytes_per_second(const NetworkUsage &previous, const NetworkUsage &current,
                                                          const std::string &interface_name = {});
    [[nodiscard]] double network_transmit_bytes_per_second(const NetworkUsage &previous, const NetworkUsage &current,
                                                           const std::string &interface_name = {});
    [[nodiscard]] DiskUsage current_disk_usage(const std::string &path = ".");
    [[nodiscard]] ResourceUsage current_resource_usage();
}

#endif
