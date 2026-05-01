#include "ssh_cli_client.h"
#include "ssh_cli_transport_loopback.h"
#include <iostream>
#include <memory>

using namespace yuan::libs::ssh_cli;

int main()
{
    auto transport = std::make_unique<SshCliTransportLoopback>();
    auto *transport_ptr = transport.get();
    SshCliClient client(std::move(transport));

    SshCliConnectionOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 22;
    opts.username = "demo";

    if (!client.connect(opts)) {
        std::cerr << "connect failed: " << client.last_error() << std::endl;
        return 1;
    }

    if (!client.authenticate_password("pass")) {
        std::cerr << "auth failed: " << client.last_error() << std::endl;
        return 1;
    }

    if (!client.open_shell()) {
        std::cerr << "shell open failed: " << client.last_error() << std::endl;
        return 1;
    }

    auto shell = client.create_shell_session();
    if (!shell) {
        std::cerr << "failed to create shell session handle" << std::endl;
        return 1;
    }
    if (!shell->is_alive()) {
        std::cerr << "shell handle should be alive after open_shell" << std::endl;
        return 1;
    }

    std::string probe_output;
    if (!client.run_command("echo loopback-probe", &probe_output)) {
        std::cerr << "run command failed: " << client.last_error() << std::endl;
        return 1;
    }
    if (probe_output.find("loopback:echo loopback-probe") == std::string::npos) {
        std::cerr << "loopback command probe output mismatch" << std::endl;
        return 1;
    }

    if (!client.send_stdin("echo loopback\n")) {
        std::cerr << "stdin send failed: " << client.last_error() << std::endl;
        return 1;
    }

    shell->close();
    if (shell->is_alive()) {
        std::cerr << "closed shell session handle should not report alive" << std::endl;
        return 1;
    }

    if (transport_ptr->captured_stdin().find("echo loopback") == std::string::npos) {
        std::cerr << "loopback transport did not capture stdin" << std::endl;
        return 1;
    }

    std::cout << "ssh cli loopback test passed" << std::endl;
    return 0;
}
