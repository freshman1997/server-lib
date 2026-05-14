#include "dns_server.h"
#include "dns_client.h"

#include "nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct RecordConfig
{
    std::string name;
    std::string type = "A";
    std::string value;
};

struct ServerConfig
{
    int port = 5353;
    std::vector<RecordConfig> records;
};

std::atomic<bool> g_running{ true };

void signal_handler(int)
{
    g_running.store(false);
}

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

std::filesystem::path default_config_path()
{
    const auto env_path = read_env_string("YUAN_DNS_CONFIG", "");
    if (!env_path.empty()) {
        return std::filesystem::path(env_path);
    }
    if (std::filesystem::exists(std::filesystem::path("release/dns/config.json"))) {
        return std::filesystem::path("release/dns/config.json");
    }
    return std::filesystem::path("config.json");
}

void print_usage(const char *program)
{
    std::cout
        << "release_dns_server\n"
        << "usage:\n"
        << "  " << program << " [--config <file>] [--port <port>]\n"
        << "  " << program << " <config.json>\n\n"
        << "options:\n"
        << "  -f, --config <file>      Read server config JSON\n"
        << "  -p, --port <port>        DNS UDP listen port\n"
        << "      --self-check-only    Run one local DNS probe and exit\n"
        << "      --version            Print version\n"
        << "  -h, --help               Show this help\n\n"
        << "env overrides:\n"
        << "  YUAN_DNS_CONFIG, YUAN_DNS_PORT\n";
}

bool parse_dns_type(const std::string &text, yuan::net::dns::DnsType &type)
{
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        if (ch >= 'a' && ch <= 'z') {
            normalized.push_back(static_cast<char>(ch - 'a' + 'A'));
        } else {
            normalized.push_back(ch);
        }
    }

    if (normalized == "A") {
        type = yuan::net::dns::DnsType::A;
        return true;
    }
    if (normalized == "AAAA") {
        type = yuan::net::dns::DnsType::AAAA;
        return true;
    }
    if (normalized == "TXT") {
        type = yuan::net::dns::DnsType::TXT;
        return true;
    }
    if (normalized == "CNAME") {
        type = yuan::net::dns::DnsType::CNAME;
        return true;
    }
    if (normalized == "NS") {
        type = yuan::net::dns::DnsType::NS;
        return true;
    }
    if (normalized == "MX") {
        type = yuan::net::dns::DnsType::MX;
        return true;
    }
    return false;
}

bool parse_int_value(const std::string &raw, int &out)
{
    try {
        std::size_t pos = 0;
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

    if (json.contains("records") && json["records"].is_array()) {
        config.records.clear();
        for (const auto &item : json["records"]) {
            if (!item.is_object()) {
                continue;
            }

            RecordConfig record;
            if (item.contains("name") && item["name"].is_string()) {
                record.name = item["name"].get<std::string>();
            }
            if (item.contains("type") && item["type"].is_string()) {
                record.type = item["type"].get<std::string>();
            }
            if (item.contains("value") && item["value"].is_string()) {
                record.value = item["value"].get<std::string>();
            }

            if (!record.name.empty() && !record.value.empty()) {
                config.records.push_back(std::move(record));
            }
        }
    }

    return true;
}

bool run_startup_self_check(int port)
{
    yuan::net::dns::DnsClient client;
    if (!client.connect("127.0.0.1", static_cast<short>(port))) {
        return false;
    }

    const bool ok = client.query("localhost", yuan::net::dns::DnsType::A, 1000);
    if (!ok) {
        client.disconnect();
        return false;
    }

    const auto &response = client.get_last_response();
    const bool has_answer = !response.get_answers().empty();
    client.disconnect();
    return has_answer;
}
} // namespace

int main(int argc, char **argv)
{
#ifndef _WIN32
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    ServerConfig config;
    int cli_port_override = 0;
    bool has_port_override = false;
    bool self_check_only = false;
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
            std::cout << "release_dns_server 1.0.0\n";
            return 0;
        }
        if (opt == "--self-check-only") {
            self_check_only = true;
            continue;
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
            if (value.empty() || !parse_int_value(value, cli_port_override)) {
                std::cerr << "invalid port: " << value << '\n';
                return 2;
            }
            has_port_override = true;
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
        config.port = cli_port_override;
    }
    config.port = read_env_int("YUAN_DNS_PORT", config.port);

    if (config.port <= 0 || config.port > 65535) {
        std::cerr << "port out of range: " << config.port << '\n';
        return 1;
    }

    if (self_check_only) {
        if (run_startup_self_check(config.port)) {
            std::cout << "self-check passed on 127.0.0.1:" << config.port << '\n';
            return 0;
        }
        std::cerr << "self-check failed on 127.0.0.1:" << config.port << '\n';
        return 1;
    }

    yuan::net::dns::DnsServer server;
    std::size_t loaded = 0;
    for (const auto &record : config.records) {
        yuan::net::dns::DnsType type = yuan::net::dns::DnsType::A;
        if (!parse_dns_type(record.type, type)) {
            std::cerr << "skip unsupported record type: " << record.type << " name=" << record.name << '\n';
            continue;
        }

        server.add_record(record.name, record.value, type);
        if (server.has_record(record.name, type, record.value)) {
            ++loaded;
        } else {
            std::cerr << "skip invalid record value for name=" << record.name << " type=" << record.type << '\n';
        }
    }

    std::cout << "release_dns_server listening on 0.0.0.0:" << config.port << '\n';
    std::cout << "loaded records: " << loaded << " (configured=" << config.records.size() << ")\n";

    std::thread server_thread([&server, &config]() {
        if (!server.serve(config.port)) {
            std::cerr << "failed to bind dns server on port " << config.port << '\n';
            g_running.store(false);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    if (!run_startup_self_check(config.port)) {
        std::cerr << "startup self-check failed on 127.0.0.1:" << config.port << '\n';
        g_running.store(false);
    }

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    return 0;
}
