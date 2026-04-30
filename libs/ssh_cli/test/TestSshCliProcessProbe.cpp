#include "ssh_cli_client.h"
#include "ssh_cli_transport_process.h"
#include "ssh_cli_test_common.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

using namespace yuan::libs::ssh_cli;

using yuan::libs::ssh_cli::test_common::enabled_by_env;

int main()
{
    if (!enabled_by_env("YUAN_RUN_SSH_CLI_PROCESS_PROBE")) {
        std::cout << "ssh cli process probe skipped (set YUAN_RUN_SSH_CLI_PROCESS_PROBE=1 to enable)." << std::endl;
        return 0;
    }

    const char *host = std::getenv("YUAN_SSH_PROBE_HOST");
    const char *port = std::getenv("YUAN_SSH_PROBE_PORT");
    const char *user = std::getenv("YUAN_SSH_PROBE_USER");
    const char *key = std::getenv("YUAN_SSH_PROBE_KEY");
    if (!host || !port || !user || !key) {
        std::cerr << "missing probe env vars (YUAN_SSH_PROBE_HOST/PORT/USER/KEY)" << std::endl;
        return 1;
    }

    auto transport = std::make_unique<SshCliTransportProcess>();
    SshCliClient client(std::move(transport));

    SshCliConnectionOptions opts;
    opts.host = host;
    opts.port = static_cast<uint16_t>(std::stoi(port));
    opts.username = user;

    if (!client.connect(opts)) {
        std::cerr << "connect failed: " << client.last_error() << std::endl;
        return 1;
    }
    if (!client.authenticate_publickey(key)) {
        std::cerr << "publickey auth failed: " << client.last_error() << std::endl;
        return 1;
    }

    std::string output;
    if (!client.run_command("echo PROCESS_PROBE_OK", &output)) {
        std::cerr << "run command failed: " << client.last_error() << std::endl;
        return 1;
    }
    if (output.find("PROCESS_PROBE_OK") == std::string::npos) {
        std::cerr << "process probe output mismatch" << std::endl;
        return 1;
    }

    if (!client.open_shell()) {
        std::cerr << "open shell failed: " << client.last_error() << std::endl;
        return 1;
    }
    auto shell = client.create_shell_session();
    if (!shell) {
        std::cerr << "failed to create shell session handle" << std::endl;
        return 1;
    }
    if (!client.is_shell_alive()) {
        std::cerr << "shell should be alive after open_shell" << std::endl;
        return 1;
    }
    if (!shell->write("echo PROCESS_INTERACTIVE_OK\n")) {
        std::cerr << "interactive send failed: " << client.last_error() << std::endl;
        return 1;
    }

    std::string interactive_output;
    if (!shell->read_until("PROCESS_INTERACTIVE_OK", &interactive_output, 2000, 10)) {
        std::cerr << "interactive shell output missing PROCESS_INTERACTIVE_OK" << std::endl;
        return 1;
    }

    if (!shell->write("sleep 5\n")) {
        std::cerr << "interactive sleep command failed: " << client.last_error() << std::endl;
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    if (!shell->send_signal("INT")) {
        std::cerr << "interactive signal failed: " << client.last_error() << std::endl;
        return 1;
    }
    if (!shell->is_alive()) {
        std::cerr << "shell handle should remain alive after INT" << std::endl;
        return 1;
    }
    if (!client.is_shell_alive()) {
        std::cerr << "shell should remain alive after INT signal" << std::endl;
        return 1;
    }

    if (!shell->write("echo AFTER_SIGNAL\nexit\n")) {
        std::cerr << "interactive command after signal failed: " << client.last_error() << std::endl;
        return 1;
    }
    std::string after_signal_output;
    if (!shell->read_until("AFTER_SIGNAL", &after_signal_output, 2000, 10)) {
        std::cerr << "interactive output missing AFTER_SIGNAL after INT" << std::endl;
        return 1;
    }

    std::cout << "ssh cli process probe passed" << std::endl;
    return 0;
}
