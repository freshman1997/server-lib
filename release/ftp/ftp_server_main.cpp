#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include "nlohmann/json.hpp"

#include "net/runtime/network_runtime.h"
#include "server/context.h"
#include "server/ftp_server.h"

namespace
{
    volatile std::sig_atomic_t g_running = 1;

    void signal_handler(int)
    {
        g_running = 0;
    }

    struct ServerConfig
    {
        int port = 2121;
        std::string root_dir = ".";
        int passive_port_start = 20000;
        int passive_port_end = 21000;
        std::string username = "tester";
        std::string password = "secret";
    };

    std::string read_env_string(const char *name, const std::string &default_value = {})
    {
        const char *raw = std::getenv(name);
        return raw ? std::string(raw) : default_value;
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
        if (json.contains("root_dir") && json["root_dir"].is_string()) {
            config.root_dir = json["root_dir"].get<std::string>();
        }
        if (json.contains("passive_port_start") && json["passive_port_start"].is_number_integer()) {
            config.passive_port_start = json["passive_port_start"].get<int>();
        }
        if (json.contains("passive_port_end") && json["passive_port_end"].is_number_integer()) {
            config.passive_port_end = json["passive_port_end"].get<int>();
        }
        if (json.contains("username") && json["username"].is_string()) {
            config.username = json["username"].get<std::string>();
        }
        if (json.contains("password") && json["password"].is_string()) {
            config.password = json["password"].get<std::string>();
        }

        return true;
    }

    std::filesystem::path default_config_path()
    {
        const auto env_path = read_env_string("YUAN_FTP_CONFIG", "");
        if (!env_path.empty()) {
            return std::filesystem::path(env_path);
        }
        if (std::filesystem::exists(std::filesystem::path("release/ftp/config.json"))) {
            return std::filesystem::path("release/ftp/config.json");
        }
        return std::filesystem::path("config.json");
    }

    void print_usage(const char *program)
    {
        std::cout
            << "release_ftp_server\n"
            << "usage:\n"
            << "  " << program << " [--config <file>] [options]\n"
            << "  " << program << " <config.json>\n\n"
            << "options:\n"
            << "  -f, --config <file>      Read server config JSON\n"
            << "  -p, --port <port>        FTP listen port\n"
            << "      --root <dir>         FTP root directory\n"
            << "      --user <name>        Username for PASS auth\n"
            << "      --password <pass>    Password for PASS auth\n"
            << "      --pasv-start <port>  Passive mode start port\n"
            << "      --pasv-end <port>    Passive mode end port\n"
            << "      --version            Print version\n"
            << "  -h, --help               Show this help\n\n"
            << "env overrides:\n"
            << "  YUAN_FTP_CONFIG, YUAN_FTP_PORT, YUAN_FTP_ROOT, YUAN_FTP_USER,\n"
            << "  YUAN_FTP_PASSWORD, YUAN_FTP_PASV_START, YUAN_FTP_PASV_END\n";
    }
}

int main(int argc, char **argv)
{
#ifndef _WIN32
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    ServerConfig config;
    ServerConfig cli_overrides;
    bool has_port_override = false;
    bool has_root_override = false;
    bool has_user_override = false;
    bool has_password_override = false;
    bool has_pasv_start_override = false;
    bool has_pasv_end_override = false;
    std::filesystem::path config_path = default_config_path();

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
            print_usage(argv[0]);
            return 0;
        }
        if (opt == "--version") {
            std::cout << "release_ftp_server 1.0.0\n";
            return 0;
        }
        if (opt == "-f" || opt == "--config") {
            const auto value = need_value(opt);
            if (value.empty()) {
                return 2;
            }
            config_path = value;
            continue;
        }
        if (opt == "-p" || opt == "--port") {
            const auto value = need_value(opt);
            if (value.empty() || !parse_int_value(value, cli_overrides.port)) {
                std::cerr << "invalid port: " << value << '\n';
                return 2;
            }
            has_port_override = true;
            continue;
        }
        if (opt == "--root") {
            cli_overrides.root_dir = need_value(opt);
            if (cli_overrides.root_dir.empty()) {
                return 2;
            }
            has_root_override = true;
            continue;
        }
        if (opt == "--user") {
            cli_overrides.username = need_value(opt);
            has_user_override = true;
            continue;
        }
        if (opt == "--password") {
            cli_overrides.password = need_value(opt);
            has_password_override = true;
            continue;
        }
        if (opt == "--pasv-start") {
            const auto value = need_value(opt);
            if (value.empty() || !parse_int_value(value, cli_overrides.passive_port_start)) {
                std::cerr << "invalid passive start port: " << value << '\n';
                return 2;
            }
            has_pasv_start_override = true;
            continue;
        }
        if (opt == "--pasv-end") {
            const auto value = need_value(opt);
            if (value.empty() || !parse_int_value(value, cli_overrides.passive_port_end)) {
                std::cerr << "invalid passive end port: " << value << '\n';
                return 2;
            }
            has_pasv_end_override = true;
            continue;
        }
        if (!opt.empty() && opt[0] == '-') {
            std::cerr << "unknown option: " << opt << '\n';
            print_usage(argv[0]);
            return 2;
        }

        config_path = opt;
    }

    if (std::filesystem::exists(config_path)) {
        std::string error;
        if (!load_config_file(config_path, config, error)) {
            std::cerr << error << '\n';
            return 1;
        }
    }

    if (has_port_override) {
        config.port = cli_overrides.port;
    }
    if (has_root_override) {
        config.root_dir = cli_overrides.root_dir;
    }
    if (has_user_override) {
        config.username = cli_overrides.username;
    }
    if (has_password_override) {
        config.password = cli_overrides.password;
    }
    if (has_pasv_start_override) {
        config.passive_port_start = cli_overrides.passive_port_start;
    }
    if (has_pasv_end_override) {
        config.passive_port_end = cli_overrides.passive_port_end;
    }

    config.port = read_env_int("YUAN_FTP_PORT", config.port);
    config.root_dir = read_env_string("YUAN_FTP_ROOT", config.root_dir);
    config.username = read_env_string("YUAN_FTP_USER", config.username);
    config.password = read_env_string("YUAN_FTP_PASSWORD", config.password);
    config.passive_port_start = read_env_int("YUAN_FTP_PASV_START", config.passive_port_start);
    config.passive_port_end = read_env_int("YUAN_FTP_PASV_END", config.passive_port_end);

    if (config.port <= 0 || config.port > 65535) {
        std::cerr << "port out of range: " << config.port << '\n';
        return 1;
    }
    if (config.passive_port_start <= 0 || config.passive_port_start > 65535 ||
        config.passive_port_end <= 0 || config.passive_port_end > 65535 ||
        config.passive_port_start > config.passive_port_end) {
        std::cerr << "invalid passive port range\n";
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(config.root_dir, ec);
    if (ec) {
        std::cerr << "failed to prepare root dir: " << config.root_dir << '\n';
        return 1;
    }

    auto ctx = yuan::net::ftp::ServerContext::get_instance();
    ctx->set_server_work_dir(std::filesystem::path(config.root_dir).lexically_normal().generic_string());
    ctx->set_stream_port_range(static_cast<short>(config.passive_port_start), static_cast<short>(config.passive_port_end));
    ctx->set_auth_credential(config.username, config.password);

    std::cout << "release_ftp_server listening on 0.0.0.0:" << config.port << '\n';
    std::cout << "root dir: " << std::filesystem::path(config.root_dir).lexically_normal().generic_string() << '\n';
    std::cout << "passive ports: " << config.passive_port_start << "-" << config.passive_port_end << '\n';
    std::cout << "auth user: " << (config.username.empty() ? "(disabled)" : config.username) << '\n';

    yuan::net::NetworkRuntime runtime;
    yuan::net::ftp::FtpServer server;
    if (!server.serve(config.port, runtime)) {
        std::cerr << "failed to start ftp server on port " << config.port << '\n';
        return 1;
    }

    std::thread runtime_thread([&runtime]() {
        runtime.run();
    });

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    server.quit();
    if (runtime_thread.joinable()) {
        runtime_thread.join();
    }
    return 0;
}
