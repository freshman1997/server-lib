#include "ssh.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace yuan::net::ssh;

namespace
{
    class InteractiveStressHandler final : public SshHandler
    {
    public:
        SshAuthResult on_authenticate(SshSession *,
                                      const std::string & username,
                                      const std::string & method,
                                      const SshAuthCredentials & credentials) override
        {
            if (method != "password") {
                return SshAuthResult::FAILURE;
            }
            if (username != "cli") {
                return SshAuthResult::FAILURE;
            }
            return credentials.password == "cli-pass"
                       ? SshAuthResult::SUCCESS
                       : SshAuthResult::FAILURE;
        }

        bool on_channel_open(SshSession *, const std::string & channel_type, SshChannel *) override
        {
            return channel_type == SSH_CHANNEL_SESSION;
        }

        bool on_pty_request(SshSession *,
                            SshChannel *,
                            const std::string &,
                            uint32_t,
                            uint32_t,
                            uint32_t,
                            uint32_t,
                            const std::vector<uint8_t> &) override
        {
            return true;
        }

        bool on_shell_request(SshSession *, SshChannel *) override
        {
            return true;
        }
    };

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

    int run_command(const std::string & command)
    {
        return std::system(command.c_str());
    }

    std::filesystem::path find_release_cli_binary()
    {
        const auto cwd = std::filesystem::current_path();
        const auto candidate_from_build = cwd / "release" / "ssh" / "release_ssh_cli";
        if (std::filesystem::exists(candidate_from_build)) {
            return candidate_from_build;
        }

        const auto candidate_from_repo = cwd / "build" / "release" / "ssh" / "release_ssh_cli";
        if (std::filesystem::exists(candidate_from_repo)) {
            return candidate_from_repo;
        }

        return {};
    }

    bool file_contains(const std::filesystem::path & path, const std::string & needle)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return text.find(needle) != std::string::npos;
    }
}

int main()
{
    const char * smoke_env = std::getenv("YUAN_RUN_RELEASE_SSH_CLI_INTERACTIVE_STRESS_SMOKE");
    if (!smoke_env || std::string(smoke_env) != "1") {
        std::cout << "release_ssh_cli interactive stress smoke skipped (set YUAN_RUN_RELEASE_SSH_CLI_INTERACTIVE_STRESS_SMOKE=1)." << std::endl;
        return 0;
    }

    const auto cli_bin = find_release_cli_binary();
    if (cli_bin.empty()) {
        std::cout << "release_ssh_cli binary not found; skipping interactive stress smoke." << std::endl;
        return 0;
    }

    const auto smoke_dir = std::filesystem::current_path() / ".tmp_release_ssh_cli_interactive_stress_smoke";
    std::error_code ec;
    std::filesystem::remove_all(smoke_dir, ec);
    std::filesystem::create_directories(smoke_dir, ec);
    if (ec) {
        std::cerr << "failed to create smoke dir: " << ec.message() << std::endl;
        return 1;
    }

    const auto host_key = smoke_dir / "ssh_host_ed25519_key";
    const auto known_hosts = smoke_dir / "known_hosts";
    const auto out_file = smoke_dir / "interactive_stress_out.txt";

    SshHostKeyProvider host_key_provider;
    if (!host_key_provider.generate_key(SshHostKeyType::ED25519, host_key.string())) {
        std::cerr << "failed to generate host key" << std::endl;
        return 1;
    }

    SshServerConfig config;
    config.host_key_paths = { host_key.string() };
    config.auth_methods = { "password" };
    config.enable_builtin_terminal_handler = true;

    InteractiveStressHandler handler;
    auto server = std::make_unique<SshServer>(config);
    server->set_handler(&handler);

    constexpr int port = 22448;
    if (!server->init(port)) {
        std::cerr << "failed to init ssh server" << std::endl;
        return 1;
    }

    std::thread server_thread([&server]() {
        server->serve();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(900));

    std::string script;
    script += "printf stress-start\\n";
    script += "for i in 1 2 3 4 5 6 7 8 9 10; do printf loop-$i\\n; done\\n";
    script += "printf stderr-line\\n 1>&2\\n";
    script += "printf stress-end\\n";
    script += "exit\\n";

    std::string cmd = "printf " + shell_quote(script) + " | " +
                      shell_quote(cli_bin.string()) +
                      " --host 127.0.0.1 --port " + std::to_string(port) +
                      " --user cli --password cli-pass" +
                      " --strict-host-key-checking accept-new" +
                      " --known-hosts " + shell_quote(known_hosts.string()) +
                      " > " + shell_quote(out_file.string()) + " 2>/dev/null";

    const int status = run_command(cmd);

    server->stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    if (status != 0) {
        std::cerr << "release_ssh_cli interactive stress command failed with status " << status << std::endl;
        return 1;
    }

    if (!file_contains(out_file, "stress-start") ||
        !file_contains(out_file, "loop-10") ||
        !file_contains(out_file, "stress-end")) {
        std::cerr << "release_ssh_cli interactive stress output mismatch" << std::endl;
        return 1;
    }

    std::cout << "release_ssh_cli interactive stress smoke passed." << std::endl;
    return 0;
}
