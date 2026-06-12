#include "ssh_server_config.h"

#include "nlohmann/json.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>

namespace yuan::release_ssh
{
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

    std::string read_env_string(const char *name, const std::string &default_value)
    {
        const char *raw = std::getenv(name);
        return raw ? std::string(raw) : default_value;
    }

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
            << "      --auth-failure-delay-ms <ms> Delay before auth failure reply\n"
            << "      --version               Print version\n"
            << "  -h, --help                  Show this help\n\n"
            << "env overrides:\n"
            << "  YUAN_SSH_CONFIG, YUAN_SSH_PORT, YUAN_SSH_HOST_KEY, YUAN_SSH_USER,\n"
            << "  YUAN_SSH_PASSWORD, YUAN_SSH_BANNER, YUAN_SSH_SFTP_ROOT,\n"
            << "  YUAN_SSH_ENABLE_PUBLICKEY_AUTH, YUAN_SSH_ENABLE_PASSWORD_AUTH,\n"
            << "  YUAN_SSH_ENABLE_SFTP, YUAN_SSH_ENABLE_PORT_FORWARD,\n"
            << "  YUAN_SSH_AUTH_FAILURE_DELAY_MS\n";
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
        if (json.contains("auth_failure_delay_ms") && json["auth_failure_delay_ms"].is_number_integer()) {
            config.auth_failure_delay_ms = json["auth_failure_delay_ms"].get<int>();
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
            } else if (opt == "--auth-failure-delay-ms") {
                const auto value = need_value(opt);
                if (value.empty() || !parse_int_value(value, args.overrides.auth_failure_delay_ms) ||
                    args.overrides.auth_failure_delay_ms < 0) {
                    std::cerr << "invalid auth failure delay: " << value << '\n';
                    return false;
                }
                args.has_auth_failure_delay = true;
            } else if (opt == "--password-auth" || opt == "--publickey-auth" ||
                       opt == "--sftp" || opt == "--port-forward") {
                const auto value = need_value(opt);
                bool enabled = false;
                if (!parse_bool_value(value, enabled)) {
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
        if (args.has_port) config.port = args.overrides.port;
        if (args.has_host_key) config.host_key_path = args.overrides.host_key_path;
        if (args.has_user) config.username = args.overrides.username;
        if (args.has_password) config.password = args.overrides.password;
        if (args.has_banner) config.banner = args.overrides.banner;
        if (args.has_sftp_root) config.sftp_root_dir = args.overrides.sftp_root_dir;
        if (args.has_password_auth) config.enable_password_auth = args.overrides.enable_password_auth;
        if (args.has_publickey_auth) config.enable_publickey_auth = args.overrides.enable_publickey_auth;
        if (args.has_sftp) config.enable_sftp = args.overrides.enable_sftp;
        if (args.has_port_forward) config.enable_port_forwarding = args.overrides.enable_port_forwarding;
        if (args.has_auth_failure_delay) config.auth_failure_delay_ms = args.overrides.auth_failure_delay_ms;
    }

    void apply_env_overrides(ServerConfig &config)
    {
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
        config.auth_failure_delay_ms = read_env_int("YUAN_SSH_AUTH_FAILURE_DELAY_MS", config.auth_failure_delay_ms);
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

    bool validate_config(const ServerConfig &config, std::string &error)
    {
        if (config.username.empty() && config.enable_password_auth) {
            error = "username cannot be empty";
            return false;
        }
        if (config.port <= 0 || config.port > 65535) {
            error = "port out of range: " + std::to_string(config.port);
            return false;
        }
        if (!config.enable_publickey_auth && !config.enable_password_auth) {
            error = "at least one auth method must be enabled";
            return false;
        }
        return true;
    }
}
