#include "ssh_cli_client.h"
#include <iostream>

using namespace yuan::libs::ssh_cli;

int main()
{
    SshCliClient client;
    SshCliConnectionOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 22;
    opts.username = "demo";

    if (!client.connect(opts)) {
        std::cerr << "connect failed: " << client.last_error() << std::endl;
        return 1;
    }

    if (client.create_shell_session()) {
        std::cerr << "shell session should not be created before open_shell" << std::endl;
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
        std::cerr << "shell session should be created after open_shell" << std::endl;
        return 1;
    }
    if (!shell->is_alive()) {
        std::cerr << "shell session should report alive after open_shell" << std::endl;
        return 1;
    }

    std::string cmd_output;
    if (!client.run_command("echo probe", &cmd_output)) {
        std::cerr << "run command failed: " << client.last_error() << std::endl;
        return 1;
    }
    if (cmd_output.find("stub:echo probe") == std::string::npos) {
        std::cerr << "run command output mismatch" << std::endl;
        return 1;
    }

    if (!client.send_stdin("echo ok\n")) {
        std::cerr << "stdin send failed: " << client.last_error() << std::endl;
        return 1;
    }

    shell->close(false);
    if (shell->is_alive()) {
        std::cerr << "shell session should not report alive after close(false)" << std::endl;
        return 1;
    }

    const auto st = client.state();
    if (!st.connected || !st.authenticated || !st.shell_open) {
        std::cerr << "unexpected client state" << std::endl;
        return 1;
    }

    client.close();
    std::cout << "ssh cli stub test passed" << std::endl;
    return 0;
}
