#include "server/ssh_server_config.h"
#include "server/ssh_server_handler.h"

#include "hostkey/ssh_host_key_provider.h"
#include "ssh_server.h"

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <system_error>
#include <thread>

namespace
{
    volatile std::sig_atomic_t g_running = 1;

    void signal_handler(int)
    {
        g_running = 0;
    }

    bool ensure_host_key(const std::string &host_key_path)
    {
        if (!host_key_path.empty()) {
            std::error_code ec;
            const auto parent = std::filesystem::path(host_key_path).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
            }
        }

        yuan::net::ssh::SshHostKeyProvider host_key_provider;
        return host_key_provider.load_or_generate(host_key_path, yuan::net::ssh::SshHostKeyType::ED25519);
    }

    yuan::net::ssh::SshServerConfig make_ssh_config(const yuan::release_ssh::ServerConfig &config)
    {
        yuan::net::ssh::SshServerConfig ssh_config;
        ssh_config.port = static_cast<uint16_t>(config.port);
        if (!config.host_key_path.empty()) {
            ssh_config.host_key_paths.push_back(config.host_key_path);
        }
        ssh_config.banner = config.banner;
        ssh_config.sftp_root_dir = config.sftp_root_dir;
        ssh_config.software_version = config.software_version;
        ssh_config.enable_sftp = config.enable_sftp;
        ssh_config.enable_port_forwarding = config.enable_port_forwarding;
        ssh_config.idle_timeout_ms = 1000;
        ssh_config.max_sessions = 1024;
        ssh_config.max_channels_per_session = 64;
        ssh_config.max_auth_attempts = 6;
        ssh_config.auth_timeout_ms = 60000;
        ssh_config.auth_failure_delay_ms = static_cast<uint32_t>(std::max(0, config.auth_failure_delay_ms));
        ssh_config.auth_methods.clear();
        if (config.enable_publickey_auth) {
            ssh_config.auth_methods.push_back("publickey");
        }
        if (config.enable_password_auth) {
            ssh_config.auth_methods.push_back("password");
        }
        return ssh_config;
    }

    void print_startup_summary(const yuan::release_ssh::ServerConfig &config,
                               const std::filesystem::path &config_path)
    {
        std::cout << "release_ssh_server listening on 0.0.0.0:" << config.port << '\n';
        std::cout << "auth methods: publickey=" << (config.enable_publickey_auth ? "on" : "off")
                  << ", password=" << (config.enable_password_auth ? "on" : "off") << '\n';
        if (config.enable_password_auth) {
            std::cout << "login user=" << config.username << " (password auth)" << '\n';
        }
        if (config.enable_publickey_auth) {
            std::cout << "authorized_keys: $YUAN_SSH_AUTHORIZED_KEYS or ~/.ssh/authorized_keys" << '\n';
        }
        if (!config_path.empty()) {
            std::cout << "config path: " << config_path.string() << '\n';
        }
        std::cout << "env: YUAN_SSH_CONFIG, YUAN_SSH_PORT, YUAN_SSH_HOST_KEY, YUAN_SSH_USER, "
                  << "YUAN_SSH_PASSWORD, YUAN_SSH_BANNER, YUAN_SSH_SFTP_ROOT, "
                  << "YUAN_SSH_ENABLE_PUBLICKEY_AUTH, YUAN_SSH_ENABLE_PASSWORD_AUTH, "
                  << "YUAN_SSH_AUTHORIZED_KEYS, YUAN_SSH_ENABLE_SFTP, YUAN_SSH_ENABLE_PORT_FORWARD" << '\n';
    }
}

int main(int argc, char **argv)
{
#ifndef _WIN32
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    yuan::release_ssh::ServerCliArgs cli_args;
    if (!yuan::release_ssh::parse_cli_args(argc, argv, cli_args)) {
        yuan::release_ssh::print_usage(argv[0]);
        return 2;
    }
    if (cli_args.help) {
        yuan::release_ssh::print_usage(argv[0]);
        return 0;
    }
    if (cli_args.version) {
        std::cout << "release_ssh_server " << yuan::release_ssh::ServerConfig{}.software_version << '\n';
        return 0;
    }

    yuan::release_ssh::ServerConfig config;
    const auto config_path = cli_args.config_path;
    if (std::filesystem::exists(config_path)) {
        std::string error;
        if (!yuan::release_ssh::load_config_file(config_path, config, error)) {
            std::cerr << error << '\n';
            return 1;
        }
    }
    yuan::release_ssh::apply_cli_overrides(cli_args, config);
    yuan::release_ssh::apply_env_overrides(config);

    std::string error;
    if (!yuan::release_ssh::validate_config(config, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::filesystem::path config_base_dir;
    if (!config_path.empty() && std::filesystem::exists(config_path)) {
        config_base_dir = config_path.parent_path();
    }
    const auto resolved_host_key_path = yuan::release_ssh::resolve_path_with_base(
        config.host_key_path,
        config_base_dir);
    if (!resolved_host_key_path.empty()) {
        config.host_key_path = resolved_host_key_path.lexically_normal().string();
    }

    if (!ensure_host_key(config.host_key_path)) {
        std::cerr << "failed to load or generate host key: " << config.host_key_path << '\n';
        return 1;
    }

    auto ssh_server = std::make_unique<yuan::net::ssh::SshServer>(make_ssh_config(config));
    auto ssh_handler = std::make_shared<yuan::release_ssh::StaticCredentialSshHandler>(
        config.username,
        config.password,
        config.enable_password_auth,
        config.enable_port_forwarding);

    ssh_server->set_handler(ssh_handler.get());
    if (!ssh_server->init(config.port)) {
        std::cerr << "failed to init ssh server\n";
        return 1;
    }

    ssh_server->serve();
    print_startup_summary(config, config_path);

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ssh_server->stop();
    return 0;
}
