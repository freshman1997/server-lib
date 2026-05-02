#include "hostkey/ssh_host_key_provider.h"
#include "ssh_server.h"
#include "ssh_handler.h"

#include "nlohmann/json.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <system_error>
#include <string>
#include <thread>

namespace
{
    volatile std::sig_atomic_t g_running = 1;

    void signal_handler(int)
    {
        g_running = 0;
    }

    int read_env_int(const char *name, int default_value)
    {
        const char *raw = std::getenv(name);
        if (!raw || *raw == '\0') {
            return default_value;
        }
        try {
            return std::stoi(raw);
        } catch (...) {
            return default_value;
        }
    }

    bool read_env_bool(const char *name, bool default_value)
    {
        const char *raw = std::getenv(name);
        if (!raw || *raw == '\0') {
            return default_value;
        }
        const std::string value(raw);
        if (value == "1" || value == "true" || value == "yes" || value == "on") {
            return true;
        }
        if (value == "0" || value == "false" || value == "no" || value == "off") {
            return false;
        }
        return default_value;
    }

    std::string read_env_string(const char *name, const std::string &default_value = {})
    {
        const char *raw = std::getenv(name);
        return raw ? std::string(raw) : default_value;
    }

    struct ServerConfig
    {
        int port = 2222;
        std::string app_name = "release-ssh-server";
        std::string host_key_path = "hostkey/ssh_host_ed25519_key";
        std::string banner = "Welcome to Yuan SSH release server";
        std::string sftp_root_dir = ".";
        std::string software_version = "YuanSSH_Release_1.0";
        std::string username = "yuan";
        std::string password = "yuan";
        bool enable_publickey_auth = true;
        bool enable_password_auth = true;
        bool enable_sftp = true;
        bool enable_port_forwarding = true;
    };

    void print_usage(const char *program)
    {
        std::cout
            << "release_ssh_server\n"
            << "usage:\n"
            << "  " << program << " [--config <file>] [options]\n"
            << "  " << program << " <config.json>\n\n"
            << "options:\n"
            << "  -f, --config <file>          Read server config JSON\n"
            << "  -p, --port <port>           Listen port\n"
            << "      --host-key <file>       Host key path\n"
            << "      --user <name>           Static password-auth username\n"
            << "      --password <password>   Static password-auth password\n"
            << "      --banner <text>         Login banner\n"
            << "      --sftp-root <dir>       SFTP root directory\n"
            << "      --password-auth <bool>  Enable password auth\n"
            << "      --publickey-auth <bool> Enable publickey auth\n"
            << "      --sftp <bool>           Enable SFTP subsystem\n"
            << "      --port-forward <bool>   Enable port forwarding\n"
            << "      --version               Print version\n"
            << "  -h, --help                  Show this help\n\n"
            << "env overrides:\n"
            << "  YUAN_SSH_CONFIG, YUAN_SSH_PORT, YUAN_SSH_HOST_KEY, YUAN_SSH_USER,\n"
            << "  YUAN_SSH_PASSWORD, YUAN_SSH_BANNER, YUAN_SSH_SFTP_ROOT,\n"
            << "  YUAN_SSH_ENABLE_PUBLICKEY_AUTH, YUAN_SSH_ENABLE_PASSWORD_AUTH,\n"
            << "  YUAN_SSH_ENABLE_SFTP, YUAN_SSH_ENABLE_PORT_FORWARD\n";
    }

    bool parse_bool_value(const std::string &raw, bool &out)
    {
        if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") {
            out = true;
            return true;
        }
        if (raw == "0" || raw == "false" || raw == "no" || raw == "off") {
            out = false;
            return true;
        }
        return false;
    }

    bool parse_int_value(const std::string &raw, int &out)
    {
        try {
            size_t pos = 0;
            const int value = std::stoi(raw, &pos);
            if (pos != raw.size()) {
                return false;
            }
            out = value;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool load_config_file(const std::filesystem::path &path, ServerConfig &config, std::string &error)
    {
        std::ifstream in(path);
        if (!in.good()) {
            error = "cannot open config file: " + path.string();
            return false;
        }

        nlohmann::json json;
        try {
            in >> json;
        } catch (const std::exception &ex) {
            error = std::string("invalid config json: ") + ex.what();
            return false;
        }

        if (json.contains("port") && json["port"].is_number_integer()) {
            config.port = json["port"].get<int>();
        }
        if (json.contains("app_name") && json["app_name"].is_string()) {
            config.app_name = json["app_name"].get<std::string>();
        }
        if (json.contains("host_key_path") && json["host_key_path"].is_string()) {
            config.host_key_path = json["host_key_path"].get<std::string>();
        }
        if (json.contains("banner") && json["banner"].is_string()) {
            config.banner = json["banner"].get<std::string>();
        }
        if (json.contains("sftp_root_dir") && json["sftp_root_dir"].is_string()) {
            config.sftp_root_dir = json["sftp_root_dir"].get<std::string>();
        }
        if (json.contains("software_version") && json["software_version"].is_string()) {
            config.software_version = json["software_version"].get<std::string>();
        }
        if (json.contains("username") && json["username"].is_string()) {
            config.username = json["username"].get<std::string>();
        }
        if (json.contains("password") && json["password"].is_string()) {
            config.password = json["password"].get<std::string>();
        }
        if (json.contains("enable_publickey_auth") && json["enable_publickey_auth"].is_boolean()) {
            config.enable_publickey_auth = json["enable_publickey_auth"].get<bool>();
        }
        if (json.contains("enable_password_auth") && json["enable_password_auth"].is_boolean()) {
            config.enable_password_auth = json["enable_password_auth"].get<bool>();
        }
        if (json.contains("enable_sftp") && json["enable_sftp"].is_boolean()) {
            config.enable_sftp = json["enable_sftp"].get<bool>();
        }
        if (json.contains("enable_port_forwarding") && json["enable_port_forwarding"].is_boolean()) {
            config.enable_port_forwarding = json["enable_port_forwarding"].get<bool>();
        }

        return true;
    }

    std::filesystem::path default_config_path()
    {
        const auto env_path = read_env_string("YUAN_SSH_CONFIG", "");
        if (!env_path.empty()) {
            return std::filesystem::path(env_path);
        }

        const std::filesystem::path local_path("release/ssh/config.json");
        if (std::filesystem::exists(local_path)) {
            return local_path;
        }

        return std::filesystem::path("config.json");
    }

    struct ServerCliArgs
    {
        std::filesystem::path config_path;
        ServerConfig overrides;
        bool has_port = false;
        bool has_host_key = false;
        bool has_user = false;
        bool has_password = false;
        bool has_banner = false;
        bool has_sftp_root = false;
        bool has_password_auth = false;
        bool has_publickey_auth = false;
        bool has_sftp = false;
        bool has_port_forward = false;
        bool help = false;
        bool version = false;
    };

    bool parse_cli_args(int argc, char **argv, ServerCliArgs &args)
    {
        args.config_path = default_config_path();

        bool positional_config_seen = false;
        for (int i = 1; i < argc; ++i) {
            const std::string opt = argv[i];
            auto need_value = [&](const std::string &name) -> std::string {
                if (i + 1 >= argc) {
                    std::cerr << "missing value for " << name << '\n';
                    return {};
                }
                return argv[++i];
            };

            if (opt == "-h" || opt == "--help") {
                args.help = true;
                return true;
            }
            if (opt == "--version") {
                args.version = true;
                return true;
            }
            if (opt == "-f" || opt == "--config") {
                const auto value = need_value(opt);
                if (value.empty()) {
                    return false;
                }
                args.config_path = value;
            } else if (opt == "-p" || opt == "--port") {
                const auto value = need_value(opt);
                if (value.empty() || !parse_int_value(value, args.overrides.port)) {
                    std::cerr << "invalid port: " << value << '\n';
                    return false;
                }
                args.has_port = true;
            } else if (opt == "--host-key") {
                args.overrides.host_key_path = need_value(opt);
                args.has_host_key = !args.overrides.host_key_path.empty();
                if (!args.has_host_key) {
                    return false;
                }
            } else if (opt == "--user") {
                args.overrides.username = need_value(opt);
                args.has_user = true;
            } else if (opt == "--password") {
                args.overrides.password = need_value(opt);
                args.has_password = true;
            } else if (opt == "--banner") {
                args.overrides.banner = need_value(opt);
                args.has_banner = true;
            } else if (opt == "--sftp-root") {
                args.overrides.sftp_root_dir = need_value(opt);
                args.has_sftp_root = !args.overrides.sftp_root_dir.empty();
                if (!args.has_sftp_root) {
                    return false;
                }
            } else if (opt == "--password-auth" || opt == "--publickey-auth" ||
                       opt == "--sftp" || opt == "--port-forward") {
                const auto value = need_value(opt);
                bool parsed = false;
                bool enabled = false;
                parsed = parse_bool_value(value, enabled);
                if (!parsed) {
                    std::cerr << "invalid boolean for " << opt << ": " << value << '\n';
                    return false;
                }
                if (opt == "--password-auth") {
                    args.overrides.enable_password_auth = enabled;
                    args.has_password_auth = true;
                } else if (opt == "--publickey-auth") {
                    args.overrides.enable_publickey_auth = enabled;
                    args.has_publickey_auth = true;
                } else if (opt == "--sftp") {
                    args.overrides.enable_sftp = enabled;
                    args.has_sftp = true;
                } else {
                    args.overrides.enable_port_forwarding = enabled;
                    args.has_port_forward = true;
                }
            } else if (!opt.empty() && opt[0] == '-') {
                std::cerr << "unknown option: " << opt << '\n';
                return false;
            } else if (!positional_config_seen) {
                args.config_path = opt;
                positional_config_seen = true;
            } else {
                std::cerr << "unexpected positional argument: " << opt << '\n';
                return false;
            }
        }

        return true;
    }

    void apply_cli_overrides(const ServerCliArgs &args, ServerConfig &config)
    {
        if (args.has_port) {
            config.port = args.overrides.port;
        }
        if (args.has_host_key) {
            config.host_key_path = args.overrides.host_key_path;
        }
        if (args.has_user) {
            config.username = args.overrides.username;
        }
        if (args.has_password) {
            config.password = args.overrides.password;
        }
        if (args.has_banner) {
            config.banner = args.overrides.banner;
        }
        if (args.has_sftp_root) {
            config.sftp_root_dir = args.overrides.sftp_root_dir;
        }
        if (args.has_password_auth) {
            config.enable_password_auth = args.overrides.enable_password_auth;
        }
        if (args.has_publickey_auth) {
            config.enable_publickey_auth = args.overrides.enable_publickey_auth;
        }
        if (args.has_sftp) {
            config.enable_sftp = args.overrides.enable_sftp;
        }
        if (args.has_port_forward) {
            config.enable_port_forwarding = args.overrides.enable_port_forwarding;
        }
    }

    std::filesystem::path resolve_path_with_base(const std::string &raw_path,
                                                 const std::filesystem::path &base_dir)
    {
        if (raw_path.empty()) {
            return {};
        }

        std::filesystem::path path(raw_path);
        if (path.is_absolute()) {
            return path;
        }

        if (!base_dir.empty()) {
            return base_dir / path;
        }

        return std::filesystem::absolute(path);
    }

    class StaticCredentialSshHandler final : public yuan::net::ssh::SshHandler
    {
    public:
        StaticCredentialSshHandler(std::string username, std::string password, bool enable_password_auth)
            : username_(std::move(username)), password_(std::move(password)), enable_password_auth_(enable_password_auth)
        {
        }

        yuan::net::ssh::SshAuthResult on_authenticate(
            yuan::net::ssh::SshSession *,
            const std::string &username,
            const std::string &method,
            const yuan::net::ssh::SshAuthCredentials &credentials) override
        {
            if (method != "password") {
                return yuan::net::ssh::SshAuthResult::FAILURE;
            }
            if (!enable_password_auth_) {
                return yuan::net::ssh::SshAuthResult::FAILURE;
            }
            if (username != username_) {
                return yuan::net::ssh::SshAuthResult::FAILURE;
            }
            return credentials.password == password_
                       ? yuan::net::ssh::SshAuthResult::SUCCESS
                       : yuan::net::ssh::SshAuthResult::FAILURE;
        }

        bool on_channel_open(yuan::net::ssh::SshSession *, const std::string &channel_type, yuan::net::ssh::SshChannel *) override
        {
            return channel_type == yuan::net::ssh::SSH_CHANNEL_SESSION;
        }

        bool on_pty_request(yuan::net::ssh::SshSession *, yuan::net::ssh::SshChannel *, const std::string &, uint32_t, uint32_t, uint32_t, uint32_t, const std::vector<uint8_t> &) override
        {
            return true;
        }

        bool on_shell_request(yuan::net::ssh::SshSession *, yuan::net::ssh::SshChannel *) override
        {
            return true;
        }

        bool on_exec_request(yuan::net::ssh::SshSession *, yuan::net::ssh::SshChannel *, const std::string &) override
        {
            return true;
        }

        bool enable_builtin_exec_bridge() const override
        {
            return true;
        }

    private:
        std::string username_;
        std::string password_;
        bool enable_password_auth_ = true;
    };
}

int main(int argc, char **argv)
{
#ifndef _WIN32
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    ServerCliArgs cli_args;
    if (!parse_cli_args(argc, argv, cli_args)) {
        print_usage(argv[0]);
        return 2;
    }
    if (cli_args.help) {
        print_usage(argv[0]);
        return 0;
    }
    if (cli_args.version) {
        std::cout << "release_ssh_server " << ServerConfig{}.software_version << '\n';
        return 0;
    }

    ServerConfig config;
    const auto config_path = cli_args.config_path;
    if (std::filesystem::exists(config_path)) {
        std::string error;
        if (!load_config_file(config_path, config, error)) {
            std::cerr << error << '\n';
            return 1;
        }
    }
    apply_cli_overrides(cli_args, config);

    std::filesystem::path config_base_dir;
    if (!config_path.empty() && std::filesystem::exists(config_path)) {
        config_base_dir = config_path.parent_path();
    }

    config.port = read_env_int("YUAN_SSH_PORT", config.port);
    config.host_key_path = read_env_string("YUAN_SSH_HOST_KEY", config.host_key_path);
    config.username = read_env_string("YUAN_SSH_USER", config.username);
    config.password = read_env_string("YUAN_SSH_PASSWORD", config.password);
    config.banner = read_env_string("YUAN_SSH_BANNER", config.banner);
    config.sftp_root_dir = read_env_string("YUAN_SSH_SFTP_ROOT", config.sftp_root_dir);
    config.enable_publickey_auth = read_env_bool("YUAN_SSH_ENABLE_PUBLICKEY_AUTH", config.enable_publickey_auth);
    config.enable_password_auth = read_env_bool("YUAN_SSH_ENABLE_PASSWORD_AUTH", config.enable_password_auth);
    config.enable_sftp = read_env_bool("YUAN_SSH_ENABLE_SFTP", config.enable_sftp);
    config.enable_port_forwarding = read_env_bool("YUAN_SSH_ENABLE_PORT_FORWARD", config.enable_port_forwarding);

    if (config.username.empty() && config.enable_password_auth) {
        std::cerr << "username cannot be empty\n";
        return 1;
    }
    if (config.port <= 0 || config.port > 65535) {
        std::cerr << "port out of range: " << config.port << '\n';
        return 1;
    }
    if (!config.enable_publickey_auth && !config.enable_password_auth) {
        std::cerr << "at least one auth method must be enabled\n";
        return 1;
    }

    const auto resolved_host_key_path = resolve_path_with_base(config.host_key_path, config_base_dir);
    if (!resolved_host_key_path.empty()) {
        config.host_key_path = resolved_host_key_path.lexically_normal().string();
    }

    {
        if (!config.host_key_path.empty()) {
            std::error_code ec;
            const auto parent = std::filesystem::path(config.host_key_path).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
            }
        }

        yuan::net::ssh::SshHostKeyProvider host_key_provider;
        if (!host_key_provider.load_or_generate(config.host_key_path, yuan::net::ssh::SshHostKeyType::ED25519)) {
            std::cerr << "failed to load or generate host key: " << config.host_key_path << '\n';
            return 1;
        }
    }

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
    ssh_config.auth_methods.clear();
    if (config.enable_publickey_auth) {
        ssh_config.auth_methods.push_back("publickey");
    }
    if (config.enable_password_auth) {
        ssh_config.auth_methods.push_back("password");
    }

    auto ssh_server = std::make_unique<yuan::net::ssh::SshServer>(ssh_config);
    auto ssh_handler = std::make_shared<StaticCredentialSshHandler>(
        config.username,
        config.password,
        config.enable_password_auth);

    ssh_server->set_handler(ssh_handler.get());
    if (!ssh_server->init(config.port)) {
        std::cerr << "failed to init ssh server\n";
        return 1;
    }

    ssh_server->serve();

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
    std::cout << "env: YUAN_SSH_CONFIG, YUAN_SSH_PORT, YUAN_SSH_HOST_KEY, YUAN_SSH_USER, YUAN_SSH_PASSWORD, YUAN_SSH_BANNER, YUAN_SSH_SFTP_ROOT, YUAN_SSH_ENABLE_PUBLICKEY_AUTH, YUAN_SSH_ENABLE_PASSWORD_AUTH, YUAN_SSH_AUTHORIZED_KEYS, YUAN_SSH_ENABLE_SFTP, YUAN_SSH_ENABLE_PORT_FORWARD" << '\n';

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ssh_server->stop();
    return 0;
}
