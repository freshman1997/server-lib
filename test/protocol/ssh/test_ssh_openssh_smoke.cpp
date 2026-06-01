#include "ssh.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace yuan::net::ssh;

namespace
{
    class SmokeExecHandler final : public SshHandler
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

        bool on_direct_tcpip(SshSession *session,
                             SshChannel *channel,
                             const std::string &target_host,
                             uint16_t target_port) override
        {
            (void)session;
            (void)channel;
            (void)target_host;
            (void)target_port;
            return false;
        }

        bool on_exec_request(SshSession *session,
                             SshChannel *channel,
                             const std::string &command) override
        {
            (void)command;
            const std::string output = "exec-ok\n";
            std::vector<uint8_t> data(output.begin(), output.end());
            session->enqueue_outgoing(session->connection_manager().build_channel_data(channel->remote_id(), data));
            session->enqueue_outgoing(session->connection_manager().build_channel_exit_status(channel->remote_id(), 0));
            session->enqueue_outgoing(session->connection_manager().build_channel_eof(channel->remote_id()));
            session->enqueue_outgoing(session->connection_manager().build_channel_close(channel->remote_id()));
            return true;
        }
    };

    int run_command(const std::string & command)
    {
        return std::system(command.c_str());
    }

    std::string shell_quote(const std::string & value)
    {
#ifdef _WIN32
        return "\"" + value + "\"";
#else
        std::string out = "'";
        for (char c : value) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out.push_back(c);
            }
        }
        out += "'";
        return out;
#endif
    }

    std::string join_command_args(const std::vector<std::string> & args)
    {
        std::string cmd;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i != 0) {
                cmd += " ";
            }
            cmd += args[i];
        }
        return cmd;
    }

    std::string build_keygen_cmd(const std::filesystem::path & user_key)
    {
        std::vector<std::string> args = {
            "ssh-keygen",
            "-q",
            "-t", "ed25519",
            "-N", shell_quote(""),
            "-f", shell_quote(user_key.string())
        };
        std::string cmd = join_command_args(args);
#ifdef _WIN32
        return "cmd /c " + cmd + " >nul 2>nul";
#else
        return cmd + " >/dev/null 2>&1";
#endif
    }

    std::string build_sftp_cmd(const std::filesystem::path & batch_file,
                               const std::filesystem::path & known_hosts,
                               const std::filesystem::path & user_key,
                               int port)
    {
        std::vector<std::string> args = {
            "sftp",
            "-vvv",
            "-b", shell_quote(batch_file.string()),
            "-oStrictHostKeyChecking=no",
            "-oUserKnownHostsFile=" + shell_quote(known_hosts.string()),
            "-oPreferredAuthentications=publickey",
            "-oPubkeyAuthentication=yes",
            "-oBatchMode=yes",
            "-i", shell_quote(user_key.string()),
            "-P", std::to_string(port),
            "demo@127.0.0.1"
        };
        std::string cmd = join_command_args(args);
#ifdef _WIN32
        return "cmd /c " + cmd;
#else
        return cmd;
#endif
    }

    std::string build_ssh_exec_cmd(const std::filesystem::path & known_hosts,
                                   const std::filesystem::path & user_key,
                                   int port,
                                   const std::filesystem::path & exec_output)
    {
        std::vector<std::string> args = {
            "ssh",
            "-oStrictHostKeyChecking=no",
            "-oUserKnownHostsFile=" + shell_quote(known_hosts.string()),
            "-oPreferredAuthentications=publickey",
            "-oPubkeyAuthentication=yes",
            "-oBatchMode=yes",
            "-i", shell_quote(user_key.string()),
            "-p", std::to_string(port),
            "demo@127.0.0.1",
            "run-smoke-exec"
        };
        std::string cmd = join_command_args(args);
#ifdef _WIN32
        return "cmd /c " + cmd + " > " + shell_quote(exec_output.string()) + " 2>nul";
#else
        return cmd + " > " + shell_quote(exec_output.string()) + " 2>/dev/null";
#endif
    }

    bool command_exists(const std::string & command)
    {
#ifdef _WIN32
        return run_command("cmd /c where " + command + " >nul 2>nul") == 0;
#else
        return run_command("command -v " + command + " >/dev/null 2>&1") == 0;
#endif
    }

    std::filesystem::path make_smoke_dir()
    {
        const auto dir = std::filesystem::current_path() / ".tmp_ssh_openssh_smoke";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    int choose_port()
    {
        return 22330;
    }
}

int main()
{
    const char * smoke_env = std::getenv("YUAN_RUN_OPENSSH_SMOKE");
    if (!smoke_env || std::string(smoke_env) != "1") {
        std::cout << "OpenSSH smoke test skipped (set YUAN_RUN_OPENSSH_SMOKE=1 to enable)." << std::endl;
        return 0;
    }

    if (!command_exists("ssh-keygen") || !command_exists("sftp") || !command_exists("ssh")) {
        std::cout << "OpenSSH client tools are unavailable; skipping smoke test." << std::endl;
        return 0;
    }

    const char * interactive_env = std::getenv("YUAN_RUN_OPENSSH_INTERACTIVE_SMOKE");
    const bool run_interactive = interactive_env && std::string(interactive_env) == "1";

    const auto smoke_dir = make_smoke_dir();
    const auto host_key = smoke_dir / "ssh_host_rsa_key";
    const auto user_key = smoke_dir / "client_ed25519";
    const auto known_hosts = smoke_dir / "known_hosts";
    const auto batch_file = smoke_dir / "sftp_batch.txt";
    const auto downloaded = smoke_dir / "downloaded.txt";
    const auto exec_output = smoke_dir / "exec_output.txt";
    const auto shell_output = smoke_dir / "shell_output.txt";
    const auto sftp_root = smoke_dir / "root";
    const auto user_pub_key = std::filesystem::path(user_key.string() + ".pub");
    const auto auth_keys = smoke_dir / "authorized_keys";

    std::error_code ec;
    std::filesystem::create_directories(sftp_root, ec);
    if (ec) {
        std::cerr << "failed to create smoke test root: " << ec.message() << std::endl;
        return 1;
    }

    {
        std::ofstream hello_file(sftp_root / "hello.txt", std::ios::binary);
        hello_file << "hello from yuan ssh";
    }

    {
        std::ofstream batch(batch_file, std::ios::binary);
        batch << "ls /\n";
        batch << "get /hello.txt " << downloaded.string() << "\n";
        batch << "bye\n";
    }

    SshHostKeyProvider generator;
    if (!generator.generate_key(SshHostKeyType::RSA, host_key.string())) {
        std::cerr << "failed to generate host key" << std::endl;
        return 1;
    }
    if (!generator.load_key(host_key.string(), SshHostKeyType::RSA)) {
        std::cerr << "generated host key could not be loaded back" << std::endl;
        return 1;
    }
    std::cout << "host key ready" << std::endl;

    const std::string keygen_cmd = build_keygen_cmd(user_key);
    if (run_command(keygen_cmd) != 0) {
        std::cerr << "failed to generate client key via ssh-keygen" << std::endl;
        return 1;
    }

    {
        std::ifstream pub_in(user_pub_key, std::ios::binary);
        std::ofstream auth_out(auth_keys, std::ios::binary);
        auth_out << pub_in.rdbuf();
    }

    const std::string auth_env_name = "YUAN_SSH_AUTHORIZED_KEYS";
    const std::string auth_env_value = auth_keys.string();
    const char *auth_env_old = std::getenv(auth_env_name.c_str());
    std::string auth_env_old_value = auth_env_old ? std::string(auth_env_old) : std::string();
#ifdef _WIN32
    _putenv_s(auth_env_name.c_str(), auth_env_value.c_str());
#else
    setenv(auth_env_name.c_str(), auth_env_value.c_str(), 1);
#endif

    struct ScopedAuthEnvReset
    {
        std::string name;
        bool had_old = false;
        std::string old_value;
        ~ScopedAuthEnvReset()
        {
#ifdef _WIN32
            if (had_old) {
                _putenv_s(name.c_str(), old_value.c_str());
            } else {
                _putenv_s(name.c_str(), "");
            }
#else
            if (had_old) {
                setenv(name.c_str(), old_value.c_str(), 1);
            } else {
                unsetenv(name.c_str());
            }
#endif
        }
    } scoped_auth_env_reset{auth_env_name, auth_env_old != nullptr, auth_env_old_value};

    std::cout << "client key ready" << std::endl;

    SshServerConfig config;
    config.host_key_paths = { host_key.string() };
    config.host_key_algorithms = { "rsa-sha2-512", "rsa-sha2-256" };
    config.auth_methods = { "publickey" };
    config.idle_timeout_ms = 1500;
    config.enable_sftp = true;
    config.sftp_root_dir = sftp_root.string();
    config.enable_builtin_terminal_handler = true;

    int port = 0;
    bool found_bindable_port = false;
    SmokeExecHandler smoke_handler;
    for (int candidate = choose_port(); candidate < choose_port() + 10; ++candidate) {
        yuan::net::NetworkRuntime probe_runtime;
        yuan::net::AsyncListenerHost probe_listener;
        const bool can_bind = probe_listener.bind(static_cast<uint16_t>(candidate), probe_runtime);
        if (!can_bind) {
            continue;
        }
        found_bindable_port = true;
        probe_listener.close();

        auto server = std::make_unique<SshServer>(config);
        server->set_handler(&smoke_handler);
        if (server->init(candidate)) {
            port = candidate;
            std::cout << "server started on port " << port << std::endl;
            std::thread server_thread([&server]() {
                server->serve();
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(750));

            const std::string sftp_cmd = build_sftp_cmd(batch_file, known_hosts, user_key, port);

            std::cout << "launching sftp" << std::endl;
            const int sftp_status = run_command(sftp_cmd);
            std::cout << "sftp exited with " << sftp_status << std::endl;

            const std::string ssh_exec_cmd = build_ssh_exec_cmd(known_hosts, user_key, port, exec_output);
            std::cout << "launching ssh exec" << std::endl;
            const int ssh_exec_status = run_command(ssh_exec_cmd);
            std::cout << "ssh exec exited with " << ssh_exec_status << std::endl;

            int ssh_probe_status = 0;
            if (run_interactive) {
#ifdef _WIN32
                const std::string ssh_probe_cmd =
                    "cmd /c ssh -T -oStrictHostKeyChecking=no -oUserKnownHostsFile=" + shell_quote(known_hosts.string()) +
                    " -oPreferredAuthentications=publickey -oPubkeyAuthentication=yes -oBatchMode=yes" +
                    " -i " + shell_quote(user_key.string()) +
                    " -p " + std::to_string(port) +
                    " demo@127.0.0.1 exit 0 >nul 2>nul";
#else
                const std::string ssh_probe_cmd =
                    "ssh -T -oStrictHostKeyChecking=no -oUserKnownHostsFile=" + shell_quote(known_hosts.string()) +
                    " -oPreferredAuthentications=publickey -oPubkeyAuthentication=yes -oBatchMode=yes" +
                    " -i " + shell_quote(user_key.string()) +
                    " -p " + std::to_string(port) +
                    " demo@127.0.0.1 exit 0 >/dev/null 2>/dev/null";
#endif
                std::cout << "launching ssh probe (-T)" << std::endl;
                ssh_probe_status = run_command(ssh_probe_cmd);
                std::cout << "ssh probe exited with " << ssh_probe_status << std::endl;

#ifdef _WIN32
                const std::string ssh_vscode_probe_cmd =
                    "cmd /c ssh -T -oStrictHostKeyChecking=no -oUserKnownHostsFile=" + shell_quote(known_hosts.string()) +
                    " -oPreferredAuthentications=publickey -oPubkeyAuthentication=yes -oBatchMode=yes" +
                    " -i " + shell_quote(user_key.string()) +
                    " -p " + std::to_string(port) +
                    " demo@127.0.0.1 \"echo VSCODE_PROBE_OK && uname -s\" >nul 2>nul";
#else
                const std::string ssh_vscode_probe_cmd =
                    "ssh -T -oStrictHostKeyChecking=no -oUserKnownHostsFile=" + shell_quote(known_hosts.string()) +
                    " -oPreferredAuthentications=publickey -oPubkeyAuthentication=yes -oBatchMode=yes" +
                    " -i " + shell_quote(user_key.string()) +
                    " -p " + std::to_string(port) +
                    " demo@127.0.0.1 \"echo VSCODE_PROBE_OK && uname -s\" >/dev/null 2>/dev/null";
#endif
                std::cout << "launching ssh vscode-like probe" << std::endl;
                const int ssh_vscode_probe_status = run_command(ssh_vscode_probe_cmd);
                std::cout << "ssh vscode-like probe exited with " << ssh_vscode_probe_status << std::endl;
                if (ssh_vscode_probe_status != 0) {
                    std::cerr << "ssh vscode-like probe command failed with exit code " << ssh_vscode_probe_status << std::endl;
                    return 1;
                }
            }

            int ssh_interactive_status = 0;
            if (run_interactive) {
#ifdef _WIN32
                const std::string ssh_interactive_cmd =
                    "cmd /c ssh -tt -oStrictHostKeyChecking=no -oUserKnownHostsFile=" + shell_quote(known_hosts.string()) +
                    " -oPreferredAuthentications=publickey -oPubkeyAuthentication=yes -oBatchMode=yes" +
                    " -i " + shell_quote(user_key.string()) +
                    " -p " + std::to_string(port) +
                    " demo@127.0.0.1 exit > " + shell_quote(shell_output.string()) + " 2>nul";
#else
                const std::string ssh_interactive_cmd =
                    "ssh -tt -oStrictHostKeyChecking=no -oUserKnownHostsFile=" + shell_quote(known_hosts.string()) +
                    " -oPreferredAuthentications=publickey -oPubkeyAuthentication=yes -oBatchMode=yes" +
                    " -i " + shell_quote(user_key.string()) +
                    " -p " + std::to_string(port) +
                    " demo@127.0.0.1 exit > " + shell_quote(shell_output.string()) + " 2>/dev/null";
#endif
                std::cout << "launching ssh interactive" << std::endl;
                ssh_interactive_status = run_command(ssh_interactive_cmd);
                std::cout << "ssh interactive exited with " << ssh_interactive_status << std::endl;
            }

            server->stop();
            if (server_thread.joinable()) {
                server_thread.join();
            }

            if (sftp_status != 0) {
                std::cerr << "sftp smoke command failed with exit code " << sftp_status << std::endl;
                return 1;
            }

            if (ssh_exec_status != 0) {
                std::cerr << "ssh exec smoke command failed with exit code " << ssh_exec_status << std::endl;
                return 1;
            }

            if (run_interactive && ssh_interactive_status != 0) {
                std::cerr << "ssh interactive smoke command failed with exit code " << ssh_interactive_status << std::endl;
                return 1;
            }

            if (run_interactive && ssh_probe_status != 0) {
                std::cerr << "ssh probe smoke command failed with exit code " << ssh_probe_status << std::endl;
                return 1;
            }

            std::ifstream downloaded_file(downloaded, std::ios::binary);
            std::string downloaded_text((std::istreambuf_iterator<char>(downloaded_file)),
                                        std::istreambuf_iterator<char>());
            if (downloaded_text != "hello from yuan ssh") {
                std::cerr << "downloaded file content mismatch" << std::endl;
                return 1;
            }

            std::ifstream exec_output_file(exec_output, std::ios::binary);
            std::string exec_output_text((std::istreambuf_iterator<char>(exec_output_file)),
                                         std::istreambuf_iterator<char>());
            if (exec_output_text.find("exec-ok") == std::string::npos) {
                std::cerr << "ssh exec output mismatch" << std::endl;
                return 1;
            }

            break;
        }
        std::cerr << "server.init failed on bindable port " << candidate << std::endl;
    }
    if (port == 0) {
        if (!found_bindable_port) {
            std::cerr << "no bindable port found in range 22330-22339" << std::endl;
        } else {
            std::cerr << "failed to start SSH server on ports 22330-22339" << std::endl;
        }
        return 1;
    }

    std::cout << "OpenSSH SFTP smoke test passed." << std::endl;
    return 0;
}
