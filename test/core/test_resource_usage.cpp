#include "base/system/resource_usage.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    using namespace yuan::base::system;

    const auto first = current_resource_usage();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto second = current_resource_usage();

    assert(first.cpu.valid());
    assert(second.cpu.valid());
    assert(process_cpu_percent(first.cpu, second.cpu) >= 0.0);

    assert(second.memory.resident_mb() >= 0.0);
    assert(second.memory.virtual_mb() >= 0.0);
    assert(second.memory.system_used_percent() >= 0.0);

    assert(first.network.valid());
    assert(second.network.valid());
    assert(network_receive_bytes_per_second(first.network, second.network) >= 0.0);
    assert(network_transmit_bytes_per_second(first.network, second.network) >= 0.0);

    assert(second.disk.used_bytes() <= second.disk.capacity_bytes);
    assert(second.disk.used_percent() >= 0.0);

    if (!second.network.interfaces.empty()) {
        const auto &interface = second.network.interfaces.front();
        assert(second.network.find_interface(interface.name) != nullptr);
        assert(network_receive_bytes_per_second(first.network, second.network, interface.name) >= 0.0);
        assert(network_transmit_bytes_per_second(first.network, second.network, interface.name) >= 0.0);
    }

    std::cout << "resource usage test passed, interfaces=" << second.network.interfaces.size() << std::endl;
    return 0;
}
