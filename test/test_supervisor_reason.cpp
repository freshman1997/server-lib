#include "bootstrap.h"

#include <iostream>

int main()
{
    using yuan::app::SupervisorReason;

    if (std::string(yuan::app::to_string(SupervisorReason::none)) != "none") {
        std::cerr << "unexpected none reason string\n";
        return 1;
    }

    if (std::string(yuan::app::to_string(SupervisorReason::scheduled_worker_restart)) != "scheduled_worker_restart") {
        std::cerr << "unexpected scheduled_worker_restart reason string\n";
        return 1;
    }

    if (std::string(yuan::app::to_string(SupervisorReason::restart_window_limit_reached)) != "restart_window_limit_reached") {
        std::cerr << "unexpected restart_window_limit_reached reason string\n";
        return 1;
    }

    if (std::string(yuan::app::to_string(SupervisorReason::restart_limit_without_recovery_window)) != "restart_limit_without_recovery_window") {
        std::cerr << "unexpected restart_limit_without_recovery_window reason string\n";
        return 1;
    }

    if (std::string(yuan::app::to_string(SupervisorReason::supervisor_circuit_opened)) != "supervisor_circuit_opened") {
        std::cerr << "unexpected supervisor_circuit_opened reason string\n";
        return 1;
    }

    if (std::string(yuan::app::to_string(SupervisorReason::supervisor_circuit_recovered)) != "supervisor_circuit_recovered") {
        std::cerr << "unexpected supervisor_circuit_recovered reason string\n";
        return 1;
    }

    if (std::string(yuan::app::to_string(SupervisorReason::worker_restarted)) != "worker_restarted") {
        std::cerr << "unexpected worker_restarted reason string\n";
        return 1;
    }

    if (std::string(yuan::app::to_string(SupervisorReason::shutdown_complete)) != "shutdown_complete") {
        std::cerr << "unexpected shutdown_complete reason string\n";
        return 1;
    }

    std::cout << "supervisor reason test passed\n";
    return 0;
}
