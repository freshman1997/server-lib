#include "ssh_cli_client.h"
#include "ssh_cli_transport_process.h"
#include "ssh_cli_test_common.h"
#include "ssh.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

using namespace yuan::libs::ssh_cli;
using namespace yuan::net::ssh;

namespace
{
    class LocalExecHandler final : public SshHandler
    {
    public:
        bool on_channel_open(SshSession *session,
                             const std::string &channel_type,
                             SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            return channel_type == SSH_CHANNEL_SESSION;
        }

        bool on_exec_request(SshSession *session,
                             SshChannel *channel,
                             const std::string &command) override
        {
            std::string out;
            if (command.find("PROCESS_LOCAL_PROBE") != std::string::npos) {
                out = "PROCESS_LOCAL_PROBE\n";
            } else if (command.find("SECOND_LOCAL_CMD") != std::string::npos) {
                out = "SECOND_LOCAL_CMD\n";
            } else {
                out = "LOCAL_UNKNOWN\n";
            }

            std::vector<uint8_t> data(out.begin(), out.end());
            session->enqueue_outgoing(session->connection_manager().build_channel_data(channel->remote_id(), data));
            session->enqueue_outgoing(session->connection_manager().build_channel_exit_status(channel->remote_id(), 0));
            session->enqueue_outgoing(session->connection_manager().build_channel_eof(channel->remote_id()));
            session->enqueue_outgoing(session->connection_manager().build_channel_close(channel->remote_id()));
            return true;
        }
    };

}

int main()
{
    if (!yuan::libs::ssh_cli::test_common::enabled_by_env("YUAN_RUN_SSH_CLI_LOCAL")) {
        std::cout << "ssh cli local process test skipped (set YUAN_RUN_SSH_CLI_LOCAL=1 to enable)." << std::endl;
        return 0;
    }

    const auto root = std::filesystem::current_path() / ".tmp_ssh_cli_local";
    std::error_code ec;
    std::string dir_error;
    if (!yuan::libs::ssh_cli::test_common::recreate_dir(root, &dir_error)) {
        std::cerr << "failed to create temp dir: " << dir_error << std::endl;
        return 1;
    }

    const auto host_key = root / "ssh_host_rsa_key";
    const auto user_key = root / "client_ed25519";
    const auto known_hosts = root / "known_hosts";

    SshHostKeyProvider generator;
    if (!generator.generate_key(SshHostKeyType::RSA, host_key.string())) {
        std::cerr << "failed to generate host key" << std::endl;
        return 1;
    }

    if (!yuan::libs::ssh_cli::test_common::generate_ed25519_keypair(user_key)) {
        std::cerr << "failed to generate client key" << std::endl;
        return 1;
    }

    SshServerConfig config;
    config.host_key_paths = { host_key.string() };
    config.host_key_algorithms = { "rsa-sha2-512", "rsa-sha2-256" };
    config.auth_methods = { "publickey" };

    const int port = yuan::libs::ssh_cli::test_common::choose_bindable_port(22440, 40);
    if (port == 0) {
        std::cerr << "failed to find bindable local test port" << std::endl;
        return 1;
    }
    LocalExecHandler handler;
    auto server = std::make_unique<SshServer>(config);
    server->set_handler(&handler);
    if (!server->init(port)) {
        std::cerr << "failed to init ssh server for local process test" << std::endl;
        return 1;
    }

    std::thread server_thread([&server]() {
        server->serve();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    auto cleanup_and_fail = [&](const std::string &msg) {
        std::cerr << msg << std::endl;
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        std::filesystem::remove_all(root, ec);
        return 1;
    };

    auto transport = std::make_unique<SshCliTransportProcess>();
    SshCliClient client(std::move(transport));
    SshCliConnectionOptions opts;
    opts.host = "127.0.0.1";
    opts.port = static_cast<uint16_t>(port);
    opts.username = "demo";
    opts.private_key_path = user_key.string();
    opts.known_hosts_path = known_hosts.string();
    opts.strict_host_key_checking = false;
    opts.batch_mode = true;

    if (!client.connect(opts)) {
        return cleanup_and_fail("connect failed: " + client.last_error());
    }
    if (!client.authenticate_publickey(opts.private_key_path)) {
        return cleanup_and_fail("auth failed: " + client.last_error());
    }

    auto run_and_expect = [&](const std::string &cmd, const std::string &needle, const std::string &tag) {
        std::string out;
        if (!client.run_command(cmd, &out)) {
            return cleanup_and_fail(tag + " command failed: " + client.last_error());
        }
        if (out.find(needle) == std::string::npos) {
            return cleanup_and_fail(tag + " output mismatch");
        }
        return 0;
    };

    if (client.is_shell_alive()) {
        return cleanup_and_fail("shell should not be alive before open_shell");
    }

    if (run_and_expect("echo PROCESS_LOCAL_PROBE", "PROCESS_LOCAL_PROBE", "first") != 0) {
        return 1;
    }
    if (run_and_expect("echo SECOND_LOCAL_CMD", "SECOND_LOCAL_CMD", "second") != 0) {
        return 1;
    }

    if (!client.open_shell()) {
        return cleanup_and_fail("failed to open shell after command probes: " + client.last_error());
    }
    auto shell = client.create_shell_session();
    if (!shell) {
        return cleanup_and_fail("failed to create shell session handle for local interactive check");
    }
    if (!shell->write("echo LOCAL_INTERACTIVE_OK\nexit\n")) {
        return cleanup_and_fail("failed to write local interactive command: " + client.last_error());
    }
    std::string interactive_output;
    if (!shell->read_until("LOCAL_INTERACTIVE_OK", &interactive_output, 2000, 10)) {
        return cleanup_and_fail("local interactive output mismatch");
    }

    server->stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    std::filesystem::remove_all(root, ec);
    std::cout << "ssh cli local process test passed" << std::endl;
    return 0;
}
