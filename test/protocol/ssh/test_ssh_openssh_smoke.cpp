#include "ssh.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace yuan::net::ssh;

namespace
{
    int run_command(const std::string & command)
    {
        return std::system(command.c_str());
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
    if (!command_exists("ssh-keygen") || !command_exists("sftp")) {
        std::cout << "OpenSSH client tools are unavailable; skipping smoke test." << std::endl;
        return 0;
    }

    const auto smoke_dir = make_smoke_dir();
    const auto host_key = smoke_dir / "ssh_host_rsa_key";
    const auto user_key = smoke_dir / "client_ed25519";
    const auto known_hosts = smoke_dir / "known_hosts";
    const auto batch_file = smoke_dir / "sftp_batch.txt";
    const auto downloaded = smoke_dir / "downloaded.txt";
    const auto sftp_root = smoke_dir / "root";

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

#ifdef _WIN32
    const std::string keygen_cmd =
        "cmd /c ssh-keygen -q -t ed25519 -N \"\" -f " + user_key.string() + " >nul 2>nul";
#else
    const std::string keygen_cmd =
        "ssh-keygen -q -t ed25519 -N '' -f '" + user_key.string() + "' >/dev/null 2>&1";
#endif
    if (run_command(keygen_cmd) != 0) {
        std::cerr << "failed to generate client key via ssh-keygen" << std::endl;
        return 1;
    }
    std::cout << "client key ready" << std::endl;

    SshServerConfig config;
    config.host_key_paths = { host_key.string() };
    config.host_key_algorithms = { "rsa-sha2-512", "rsa-sha2-256" };
    config.auth_methods = { "publickey" };
    config.enable_sftp = true;
    config.sftp_root_dir = sftp_root.string();

    int port = 0;
    bool found_bindable_port = false;
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
        if (server->init(candidate)) {
            port = candidate;
            std::cout << "server started on port " << port << std::endl;
            std::thread server_thread([&server]() {
                server->serve();
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(750));

#ifdef _WIN32
            const std::string sftp_cmd =
                "cmd /c sftp -vvv -b " + batch_file.string() +
                " -oStrictHostKeyChecking=no -oUserKnownHostsFile=" + known_hosts.string() +
                " -oPreferredAuthentications=publickey -oPubkeyAuthentication=yes -oBatchMode=yes" +
                " -i " + user_key.string() +
                " -P " + std::to_string(port) +
                " demo@127.0.0.1";
#else
            const std::string sftp_cmd =
                "sftp -vvv -b '" + batch_file.string() + "'" +
                " -oStrictHostKeyChecking=no -oUserKnownHostsFile='" + known_hosts.string() + "'" +
                " -oPreferredAuthentications=publickey -oPubkeyAuthentication=yes -oBatchMode=yes" +
                " -i '" + user_key.string() + "'" +
                " -P " + std::to_string(port) +
                " demo@127.0.0.1";
#endif

            std::cout << "launching sftp" << std::endl;
            const int sftp_status = run_command(sftp_cmd);
            std::cout << "sftp exited with " << sftp_status << std::endl;

            server->stop();
            if (server_thread.joinable()) {
                server_thread.join();
            }

            if (sftp_status != 0) {
                std::cerr << "sftp smoke command failed with exit code " << sftp_status << std::endl;
                return 1;
            }

            std::ifstream downloaded_file(downloaded, std::ios::binary);
            std::string downloaded_text((std::istreambuf_iterator<char>(downloaded_file)),
                                        std::istreambuf_iterator<char>());
            if (downloaded_text != "hello from yuan ssh") {
                std::cerr << "downloaded file content mismatch" << std::endl;
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
