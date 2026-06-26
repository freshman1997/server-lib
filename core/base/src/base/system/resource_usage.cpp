#include "base/system/resource_usage.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iphlpapi.h>
#include <psapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/task.h>
#include <net/if_dl.h>
#endif
#endif

namespace yuan::base::system
{
    namespace
    {
        constexpr double bytes_per_mb = 1024.0 * 1024.0;

        uint64_t steady_now_us()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        }

        NetworkInterfaceUsage *find_mutable_interface(NetworkUsage &usage, const std::string &name)
        {
            const auto found = std::find_if(usage.interfaces.begin(), usage.interfaces.end(), [&](const auto &net_interface) {
                return net_interface.name == name;
            });
            return found == usage.interfaces.end() ? nullptr : &*found;
        }

#ifdef _WIN32
        uint64_t filetime_to_us(const FILETIME &file_time)
        {
            ULARGE_INTEGER value{};
            value.LowPart = file_time.dwLowDateTime;
            value.HighPart = file_time.dwHighDateTime;
            return value.QuadPart / 10ULL;
        }

        uint64_t memory_status_bytes(const DWORDLONG value)
        {
            return static_cast<uint64_t>(value);
        }
#else
        uint64_t timeval_to_us(const timeval &value)
        {
            return static_cast<uint64_t>(value.tv_sec) * 1000000ULL + static_cast<uint64_t>(value.tv_usec);
        }

        uint64_t page_size()
        {
            const auto value = sysconf(_SC_PAGESIZE);
            return value > 0 ? static_cast<uint64_t>(value) : 0ULL;
        }

        uint64_t pages_to_bytes(const long pages)
        {
            if (pages < 0) {
                return 0;
            }
            return static_cast<uint64_t>(pages) * page_size();
        }
#endif

#ifdef __linux__
        bool read_linux_process_memory(MemoryUsage &usage)
        {
            FILE *file = std::fopen("/proc/self/statm", "r");
            if (!file) {
                return false;
            }

            unsigned long long virtual_pages = 0;
            unsigned long long resident_pages = 0;
            const auto matched = std::fscanf(file, "%llu %llu", &virtual_pages, &resident_pages);
            std::fclose(file);
            if (matched != 2) {
                return false;
            }

            const auto size = page_size();
            usage.virtual_bytes = virtual_pages * size;
            usage.resident_bytes = resident_pages * size;
            return true;
        }

        void read_linux_system_memory(MemoryUsage &usage)
        {
            FILE *file = std::fopen("/proc/meminfo", "r");
            if (!file) {
                return;
            }

            char key[64]{};
            unsigned long long value_kb = 0;
            char unit[32]{};
            while (std::fscanf(file, "%63s %llu %31s", key, &value_kb, unit) == 3) {
                if (std::strcmp(key, "MemTotal:") == 0) {
                    usage.system_total_bytes = value_kb * 1024ULL;
                } else if (std::strcmp(key, "MemAvailable:") == 0) {
                    usage.system_available_bytes = value_kb * 1024ULL;
                }
            }
            std::fclose(file);
        }

        NetworkUsage read_linux_network_usage()
        {
            NetworkUsage result{};
            result.timestamp_us = steady_now_us();
            std::ifstream file("/proc/net/dev");
            if (!file.is_open()) {
                return result;
            }

            std::string line;
            std::getline(file, line);
            std::getline(file, line);
            while (std::getline(file, line)) {
                const auto separator = line.find(':');
                if (separator == std::string::npos) {
                    continue;
                }

                auto name = line.substr(0, separator);
                name.erase(std::remove_if(name.begin(), name.end(), [](const unsigned char ch) { return std::isspace(ch) != 0; }), name.end());

                std::istringstream values(line.substr(separator + 1));
                NetworkInterfaceUsage net_interface{};
                net_interface.name = name;
                uint64_t receive_drop = 0;
                uint64_t receive_fifo = 0;
                uint64_t receive_frame = 0;
                uint64_t receive_compressed = 0;
                uint64_t receive_multicast = 0;
                uint64_t transmit_drop = 0;
                uint64_t transmit_fifo = 0;
                uint64_t transmit_colls = 0;
                uint64_t transmit_carrier = 0;
                uint64_t transmit_compressed = 0;

                values >> net_interface.receive_bytes >> net_interface.receive_packets >> net_interface.receive_errors >> receive_drop
                    >> receive_fifo >> receive_frame >> receive_compressed >> receive_multicast
                    >> net_interface.transmit_bytes >> net_interface.transmit_packets >> net_interface.transmit_errors >> transmit_drop
                    >> transmit_fifo >> transmit_colls >> transmit_carrier >> transmit_compressed;
                if (!values.fail()) {
                    net_interface.loopback = net_interface.name == "lo";
                    net_interface.up = true;
                    result.interfaces.push_back(std::move(net_interface));
                }
            }
            return result;
        }
#elif defined(_WIN32)
        NetworkUsage read_windows_network_usage()
        {
            NetworkUsage result{};
            result.timestamp_us = steady_now_us();
            ULONG table_size = 0;
            if (GetIfTable(nullptr, &table_size, FALSE) != ERROR_INSUFFICIENT_BUFFER || table_size == 0) {
                return result;
            }

            std::vector<unsigned char> buffer(table_size);
            auto *table = reinterpret_cast<MIB_IFTABLE *>(buffer.data());
            if (GetIfTable(table, &table_size, FALSE) != NO_ERROR) {
                return result;
            }

            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                const auto &row = table->table[i];
                NetworkInterfaceUsage net_interface{};
                net_interface.name = row.bDescr[0] != '\0'
                    ? std::string(reinterpret_cast<const char *>(row.bDescr), row.dwDescrLen)
                    : std::to_string(row.dwIndex);
                net_interface.up = row.dwOperStatus == MIB_IF_OPER_STATUS_OPERATIONAL;
                net_interface.loopback = row.dwType == IF_TYPE_SOFTWARE_LOOPBACK;
                net_interface.receive_bytes = static_cast<uint64_t>(row.dwInOctets);
                net_interface.receive_packets = static_cast<uint64_t>(row.dwInUcastPkts + row.dwInNUcastPkts);
                net_interface.receive_errors = static_cast<uint64_t>(row.dwInErrors);
                net_interface.transmit_bytes = static_cast<uint64_t>(row.dwOutOctets);
                net_interface.transmit_packets = static_cast<uint64_t>(row.dwOutUcastPkts + row.dwOutNUcastPkts);
                net_interface.transmit_errors = static_cast<uint64_t>(row.dwOutErrors);
                result.interfaces.push_back(std::move(net_interface));
            }

            return result;
        }
#else
        NetworkUsage read_unix_network_usage()
        {
            NetworkUsage result{};
            result.timestamp_us = steady_now_us();
            ifaddrs *addresses = nullptr;
            if (getifaddrs(&addresses) != 0 || addresses == nullptr) {
                return result;
            }

            for (auto *address = addresses; address != nullptr; address = address->ifa_next) {
                if (!address->ifa_name) {
                    continue;
                }

                auto *net_interface = find_mutable_interface(result, address->ifa_name);
                if (net_interface == nullptr) {
                    NetworkInterfaceUsage created{};
                    created.name = address->ifa_name;
                    created.up = (address->ifa_flags & IFF_UP) != 0;
                    created.loopback = (address->ifa_flags & IFF_LOOPBACK) != 0;
                    result.interfaces.push_back(std::move(created));
                    net_interface = &result.interfaces.back();
                }

#if defined(__APPLE__)
                if (address->ifa_addr && address->ifa_addr->sa_family == AF_LINK && address->ifa_data) {
                    const auto *data = static_cast<const if_data *>(address->ifa_data);
                    net_interface->receive_bytes = data->ifi_ibytes;
                    net_interface->receive_packets = data->ifi_ipackets;
                    net_interface->receive_errors = data->ifi_ierrors;
                    net_interface->transmit_bytes = data->ifi_obytes;
                    net_interface->transmit_packets = data->ifi_opackets;
                    net_interface->transmit_errors = data->ifi_oerrors;
                }
#endif
            }

            freeifaddrs(addresses);
            return result;
        }
#endif
    }

    double MemoryUsage::resident_mb() const
    {
        return static_cast<double>(resident_bytes) / bytes_per_mb;
    }

    double MemoryUsage::virtual_mb() const
    {
        return static_cast<double>(virtual_bytes) / bytes_per_mb;
    }

    double MemoryUsage::system_used_percent() const
    {
        if (system_total_bytes == 0 || system_available_bytes > system_total_bytes) {
            return 0.0;
        }
        const auto used = system_total_bytes - system_available_bytes;
        return static_cast<double>(used) * 100.0 / static_cast<double>(system_total_bytes);
    }

    const NetworkInterfaceUsage *NetworkUsage::find_interface(const std::string &name) const
    {
        const auto found = std::find_if(interfaces.begin(), interfaces.end(), [&](const auto &net_interface) {
            return net_interface.name == name;
        });
        return found == interfaces.end() ? nullptr : &*found;
    }

    uint64_t NetworkUsage::total_receive_bytes() const
    {
        uint64_t total = 0;
        for (const auto &net_interface : interfaces) {
            if (!net_interface.loopback) {
                total += net_interface.receive_bytes;
            }
        }
        return total;
    }

    uint64_t NetworkUsage::total_transmit_bytes() const
    {
        uint64_t total = 0;
        for (const auto &net_interface : interfaces) {
            if (!net_interface.loopback) {
                total += net_interface.transmit_bytes;
            }
        }
        return total;
    }

    double network_receive_bytes_per_second(const NetworkUsage &previous, const NetworkUsage &current,
                                            const std::string &interface_name)
    {
        if (!previous.valid() || !current.valid() || current.timestamp_us <= previous.timestamp_us) {
            return 0.0;
        }

        const auto elapsed_seconds = static_cast<double>(current.timestamp_us - previous.timestamp_us) / 1000000.0;
        if (elapsed_seconds <= 0.0) {
            return 0.0;
        }

        if (!interface_name.empty()) {
            const auto *previous_interface = previous.find_interface(interface_name);
            const auto *current_interface = current.find_interface(interface_name);
            if (!previous_interface || !current_interface || current_interface->receive_bytes < previous_interface->receive_bytes) {
                return 0.0;
            }
            return static_cast<double>(current_interface->receive_bytes - previous_interface->receive_bytes) / elapsed_seconds;
        }

        const auto previous_bytes = previous.total_receive_bytes();
        const auto current_bytes = current.total_receive_bytes();
        return current_bytes >= previous_bytes ? static_cast<double>(current_bytes - previous_bytes) / elapsed_seconds : 0.0;
    }

    double network_transmit_bytes_per_second(const NetworkUsage &previous, const NetworkUsage &current,
                                             const std::string &interface_name)
    {
        if (!previous.valid() || !current.valid() || current.timestamp_us <= previous.timestamp_us) {
            return 0.0;
        }

        const auto elapsed_seconds = static_cast<double>(current.timestamp_us - previous.timestamp_us) / 1000000.0;
        if (elapsed_seconds <= 0.0) {
            return 0.0;
        }

        if (!interface_name.empty()) {
            const auto *previous_interface = previous.find_interface(interface_name);
            const auto *current_interface = current.find_interface(interface_name);
            if (!previous_interface || !current_interface || current_interface->transmit_bytes < previous_interface->transmit_bytes) {
                return 0.0;
            }
            return static_cast<double>(current_interface->transmit_bytes - previous_interface->transmit_bytes) / elapsed_seconds;
        }

        const auto previous_bytes = previous.total_transmit_bytes();
        const auto current_bytes = current.total_transmit_bytes();
        return current_bytes >= previous_bytes ? static_cast<double>(current_bytes - previous_bytes) / elapsed_seconds : 0.0;
    }

    uint64_t DiskUsage::used_bytes() const
    {
        return capacity_bytes >= free_bytes ? capacity_bytes - free_bytes : 0;
    }

    double DiskUsage::used_percent() const
    {
        if (capacity_bytes == 0) {
            return 0.0;
        }
        return static_cast<double>(used_bytes()) * 100.0 / static_cast<double>(capacity_bytes);
    }

    CpuTimes current_process_cpu_times()
    {
        CpuTimes result{};
        result.timestamp_us = steady_now_us();

#ifdef _WIN32
        FILETIME creation_time{};
        FILETIME exit_time{};
        FILETIME kernel_time{};
        FILETIME user_time{};
        if (GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time)) {
            result.user_us = filetime_to_us(user_time);
            result.system_us = filetime_to_us(kernel_time);
        }
#else
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            result.user_us = timeval_to_us(usage.ru_utime);
            result.system_us = timeval_to_us(usage.ru_stime);
        }
#endif

        return result;
    }

    double process_cpu_percent(const CpuTimes &previous, const CpuTimes &current)
    {
        if (!previous.valid() || !current.valid() || current.timestamp_us <= previous.timestamp_us) {
            return 0.0;
        }

        const auto cpu_delta = current.total_us() >= previous.total_us() ? current.total_us() - previous.total_us() : 0ULL;
        const auto wall_delta = current.timestamp_us - previous.timestamp_us;
        if (wall_delta == 0) {
            return 0.0;
        }
        return static_cast<double>(cpu_delta) * 100.0 / static_cast<double>(wall_delta);
    }

    MemoryUsage current_memory_usage()
    {
        MemoryUsage usage{};

#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters))) {
            usage.resident_bytes = static_cast<uint64_t>(counters.WorkingSetSize);
            usage.virtual_bytes = static_cast<uint64_t>(counters.PrivateUsage);
        }

        MEMORYSTATUSEX memory_status{};
        memory_status.dwLength = sizeof(memory_status);
        if (GlobalMemoryStatusEx(&memory_status)) {
            usage.system_total_bytes = memory_status_bytes(memory_status.ullTotalPhys);
            usage.system_available_bytes = memory_status_bytes(memory_status.ullAvailPhys);
        }
#elif defined(__linux__)
        read_linux_process_memory(usage);
        read_linux_system_memory(usage);
#else
        rusage resource_usage{};
        if (getrusage(RUSAGE_SELF, &resource_usage) == 0) {
#if defined(__APPLE__)
            usage.resident_bytes = static_cast<uint64_t>(resource_usage.ru_maxrss);
#else
            usage.resident_bytes = static_cast<uint64_t>(resource_usage.ru_maxrss) * 1024ULL;
#endif
        }

        const auto pages = sysconf(_SC_PHYS_PAGES);
        const auto available_pages = sysconf(_SC_AVPHYS_PAGES);
        usage.system_total_bytes = pages_to_bytes(pages);
        usage.system_available_bytes = pages_to_bytes(available_pages);

#if defined(__APPLE__)
        task_basic_info info{};
        mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
            usage.resident_bytes = static_cast<uint64_t>(info.resident_size);
            usage.virtual_bytes = static_cast<uint64_t>(info.virtual_size);
        }
#endif
#endif

        return usage;
    }

    NetworkUsage current_network_usage()
    {
#ifdef __linux__
        return read_linux_network_usage();
#elif defined(_WIN32)
        return read_windows_network_usage();
#else
        return read_unix_network_usage();
#endif
    }

    DiskUsage current_disk_usage(const std::string &path)
    {
        DiskUsage usage{};
        usage.path = path;

        std::error_code error;
        const auto info = std::filesystem::space(path, error);
        if (!error) {
            usage.capacity_bytes = static_cast<uint64_t>(info.capacity);
            usage.free_bytes = static_cast<uint64_t>(info.free);
            usage.available_bytes = static_cast<uint64_t>(info.available);
        }
        return usage;
    }

    ResourceUsage current_resource_usage()
    {
        ResourceUsage usage{};
        usage.cpu = current_process_cpu_times();
        usage.memory = current_memory_usage();
        usage.network = current_network_usage();
        usage.disk = current_disk_usage();
        return usage;
    }
}
