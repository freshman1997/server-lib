#include "ssh_cli_client.h"
#include "ssh_cli_shell_session.h"
#include "ssh_cli_transport_process.h"

#include <iostream>
#include <memory>
#include <string>

using namespace yuan::libs::ssh_cli;

namespace
{
    void print_usage()
    {
        std::cout << "Usage:\n"
                  << "  ssh_cli_demo --host <host> --port <port> --user <user> --key <private_key> --cmd <command>\n"
                  << "  ssh_cli_demo --host <host> --port <port> --user <user> --key <private_key> --interactive [--timeout-ms <ms>] [--poll-ms <ms>]\n"
                  << "Options:\n"
                  << "  --strict-host-key\n"
                  << "  --known-hosts <path>\n"
                  << "  --timeout-ms <ms>   interactive read timeout (default 80)\n"
                  << "  --poll-ms <ms>      interactive read poll interval (default 10)\n";
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    SshCliConnectionOptions opts;
    std::string cmd;
    bool interactive = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto read_next = [&](std::string *out) {
            if (i + 1 >= argc) {
                return false;
            }
            *out = argv[++i];
            return true;
        };

        if (arg == "--host") {
            if (!read_next(&opts.host)) {
                print_usage();
                return 1;
            }
        } else if (arg == "--port") {
            std::string p;
            if (!read_next(&p)) {
                print_usage();
                return 1;
            }
            opts.port = static_cast<uint16_t>(std::stoi(p));
        } else if (arg == "--user") {
            if (!read_next(&opts.username)) {
                print_usage();
                return 1;
            }
        } else if (arg == "--key") {
            if (!read_next(&opts.private_key_path)) {
                print_usage();
                return 1;
            }
        } else if (arg == "--cmd") {
            if (!read_next(&cmd)) {
                print_usage();
                return 1;
            }
        } else if (arg == "--interactive") {
            interactive = true;
        } else if (arg == "--strict-host-key") {
            opts.strict_host_key_checking = true;
        } else if (arg == "--known-hosts") {
            if (!read_next(&opts.known_hosts_path)) {
                print_usage();
                return 1;
            }
        } else if (arg == "--timeout-ms") {
            std::string v;
            if (!read_next(&v)) {
                print_usage();
                return 1;
            }
            opts.interactive_read_timeout_ms = std::stoi(v);
        } else if (arg == "--poll-ms") {
            std::string v;
            if (!read_next(&v)) {
                print_usage();
                return 1;
            }
            opts.interactive_poll_interval_ms = std::stoi(v);
        } else {
            print_usage();
            return 1;
        }
    }

    if (opts.username.empty() || opts.private_key_path.empty()) {
        print_usage();
        return 1;
    }

    auto transport = std::make_unique<SshCliTransportProcess>();
    SshCliClient client(std::move(transport));

    if (!client.connect(opts)) {
        std::cerr << "connect failed: " << client.last_error() << std::endl;
        return 2;
    }
    if (!client.authenticate_publickey(opts.private_key_path)) {
        std::cerr << "auth failed: " << client.last_error() << std::endl;
        return 3;
    }

    if (!interactive) {
        if (cmd.empty()) {
            std::cerr << "missing --cmd for non-interactive mode" << std::endl;
            return 1;
        }
        std::string out;
        if (!client.run_command(cmd, &out)) {
            std::cerr << "run command failed: " << client.last_error() << std::endl;
            return 4;
        }
        std::cout << out;
        return 0;
    }

    if (!client.open_shell()) {
        std::cerr << "open shell failed: " << client.last_error() << std::endl;
        return 5;
    }
    auto shell = client.create_shell_session();
    if (!shell) {
        std::cerr << "failed to create shell session" << std::endl;
        return 6;
    }

    std::cout << "interactive mode started, type lines, Ctrl-D to exit\n";
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!shell->write(line + "\n")) {
            std::cerr << "write failed: " << client.last_error() << std::endl;
            break;
        }
        std::string chunk;
        if (shell->read(&chunk) && !chunk.empty()) {
            std::cout << chunk;
        }
    }

    shell->close(true);
    return 0;
}
