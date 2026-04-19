#include "application.h"
#include "bootstrap.h"
#include "nlohmann/json.hpp"
#include "proxy_service.h"
#include "socks5_service.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

namespace
{
    std::atomic_bool g_running{ true };

    void signal_handler(int)
    {
        g_running.store(false, std::memory_order_relaxed);
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

    std::string read_env_string(const char *name, const std::string &default_value = {})
    {
        const char *raw = std::getenv(name);
        return raw ? std::string(raw) : default_value;
    }

    std::vector<std::string> split_csv(const std::string &raw)
    {
        std::vector<std::string> values;
        std::size_t start = 0;
        while (start <= raw.size()) {
            const auto comma = raw.find(',', start);
            const auto token = raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!token.empty()) {
                values.push_back(token);
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        return values;
    }

    void apply_json_config(const nlohmann::json &json, yuan::server::ProxyServiceConfig &config)
    {
        const auto *proxy = &json;
        if (json.contains("proxy") && json["proxy"].is_object()) {
            proxy = &json["proxy"];
        }

        if (proxy->contains("listen_host")) {
            config.listen_host = (*proxy)["listen_host"].get<std::string>();
        }
        if (proxy->contains("listen_port")) {
            config.port = (*proxy)["listen_port"].get<int>();
        }
        if (proxy->contains("max_active_sessions")) {
            config.max_active_sessions = (*proxy)["max_active_sessions"].get<int>();
        }
        if (proxy->contains("max_sessions_per_client")) {
            config.max_sessions_per_client = (*proxy)["max_sessions_per_client"].get<int>();
        }
        if (proxy->contains("header_timeout_ms")) {
            config.header_timeout_ms = (*proxy)["header_timeout_ms"].get<int>();
        }
        if (proxy->contains("idle_timeout_ms")) {
            config.idle_timeout_ms = (*proxy)["idle_timeout_ms"].get<int>();
        }
        if (proxy->contains("connect_timeout_ms")) {
            config.connect_timeout_ms = (*proxy)["connect_timeout_ms"].get<int>();
        }
        if (proxy->contains("drain_timeout_ms")) {
            config.drain_timeout_ms = (*proxy)["drain_timeout_ms"].get<int>();
        }
        if (proxy->contains("session_snapshot_interval_ms")) {
            config.session_snapshot_interval_ms = (*proxy)["session_snapshot_interval_ms"].get<int>();
        }
        if (proxy->contains("max_header_bytes")) {
            config.max_header_bytes = (*proxy)["max_header_bytes"].get<int>();
        }
        if (proxy->contains("basic_auth_user")) {
            config.basic_auth_user = (*proxy)["basic_auth_user"].get<std::string>();
        }
        if (proxy->contains("basic_auth_password")) {
            config.basic_auth_password = (*proxy)["basic_auth_password"].get<std::string>();
        }
        if (proxy->contains("allow_targets") && (*proxy)["allow_targets"].is_array()) {
            config.allow_targets = (*proxy)["allow_targets"].get<std::vector<std::string>>();
        }
        if (proxy->contains("deny_targets") && (*proxy)["deny_targets"].is_array()) {
            config.deny_targets = (*proxy)["deny_targets"].get<std::vector<std::string>>();
        }
    }

    struct Socks5ServiceConfigFile
    {
        bool enabled = false;
        int port = 1080;
        yuan::net::socks5::Socks5ServerConfig server_config;
    };

    void apply_json_config(const nlohmann::json &json, Socks5ServiceConfigFile &config)
    {
        const auto *node = &json;
        if (json.contains("socks5") && json["socks5"].is_object()) {
            node = &json["socks5"];
        }

        if (node->contains("enabled")) {
            config.enabled = (*node)["enabled"].get<bool>();
        }
        if (node->contains("listen_port")) {
            config.port = (*node)["listen_port"].get<int>();
        }
        if (node->contains("enable_auth")) {
            config.server_config.enable_auth = (*node)["enable_auth"].get<bool>();
        }
        if (node->contains("username")) {
            config.server_config.username = (*node)["username"].get<std::string>();
        }
        if (node->contains("password")) {
            config.server_config.password = (*node)["password"].get<std::string>();
        }
        if (node->contains("enable_connect")) {
            config.server_config.enable_connect = (*node)["enable_connect"].get<bool>();
        }
        if (node->contains("enable_bind")) {
            config.server_config.enable_bind = (*node)["enable_bind"].get<bool>();
        }
        if (node->contains("enable_udp_associate")) {
            config.server_config.enable_udp_associate = (*node)["enable_udp_associate"].get<bool>();
        }
        if (node->contains("connect_timeout_ms")) {
            config.server_config.connect_timeout_ms = (*node)["connect_timeout_ms"].get<uint32_t>();
        }
        if (node->contains("idle_timeout_ms")) {
            config.server_config.idle_timeout_ms = (*node)["idle_timeout_ms"].get<uint32_t>();
        }
        if (node->contains("max_connections")) {
            config.server_config.max_connections = (*node)["max_connections"].get<size_t>();
        }
    }

    bool load_config_file(const std::string &path,
                          yuan::server::ProxyServiceConfig &proxy_config,
                          Socks5ServiceConfigFile &socks5_config)
    {
        if (path.empty()) {
            return true;
        }

        std::ifstream input(path);
        if (!input.good()) {
            std::cerr << "failed to open proxy config: " << path << '\n';
            return false;
        }

        try {
            nlohmann::json json;
            input >> json;
            apply_json_config(json, proxy_config);
            apply_json_config(json, socks5_config);
            return true;
        } catch (const std::exception &ex) {
            std::cerr << "failed to parse proxy config " << path << ": " << ex.what() << '\n';
            return false;
        }
    }

    void apply_env_overrides(yuan::server::ProxyServiceConfig &config)
    {
        config.listen_host = read_env_string("YUAN_PROXY_LISTEN_HOST", config.listen_host);
        config.port = read_env_int("YUAN_PROXY_LISTEN_PORT", config.port);
        config.max_active_sessions = read_env_int("YUAN_PROXY_MAX_ACTIVE", config.max_active_sessions);
        config.max_sessions_per_client =
            read_env_int("YUAN_PROXY_MAX_SESSIONS_PER_CLIENT", config.max_sessions_per_client);
        config.header_timeout_ms = read_env_int("YUAN_PROXY_HEADER_TIMEOUT_MS", config.header_timeout_ms);
        config.idle_timeout_ms = read_env_int("YUAN_PROXY_IDLE_TIMEOUT_MS", config.idle_timeout_ms);
        config.connect_timeout_ms = read_env_int("YUAN_PROXY_CONNECT_TIMEOUT_MS", config.connect_timeout_ms);
        config.drain_timeout_ms = read_env_int("YUAN_PROXY_DRAIN_TIMEOUT_MS", config.drain_timeout_ms);
        config.session_snapshot_interval_ms =
            read_env_int("YUAN_PROXY_SESSION_SNAPSHOT_INTERVAL_MS", config.session_snapshot_interval_ms);
        config.max_header_bytes = read_env_int("YUAN_PROXY_MAX_HEADER_BYTES", config.max_header_bytes);

        const std::string env_user = read_env_string("YUAN_PROXY_BASIC_USER");
        const std::string env_pass = read_env_string("YUAN_PROXY_BASIC_PASS");
        if (!env_user.empty()) {
            config.basic_auth_user = env_user;
        }
        if (!env_pass.empty()) {
            config.basic_auth_password = env_pass;
        }

        const std::string env_allow = read_env_string("YUAN_PROXY_ALLOW_TARGETS");
        const std::string env_deny = read_env_string("YUAN_PROXY_DENY_TARGETS");
        if (!env_allow.empty()) {
            config.allow_targets = split_csv(env_allow);
        }
        if (!env_deny.empty()) {
            config.deny_targets = split_csv(env_deny);
        }
    }

    void apply_env_overrides(Socks5ServiceConfigFile &config)
    {
        const std::string enabled = read_env_string("YUAN_SOCKS5_ENABLED");
        if (!enabled.empty()) {
            config.enabled = enabled == "1" || enabled == "true" || enabled == "TRUE";
        }
        config.port = read_env_int("YUAN_SOCKS5_PORT", config.port);
        const std::string enable_auth = read_env_string("YUAN_SOCKS5_ENABLE_AUTH");
        if (!enable_auth.empty()) {
            config.server_config.enable_auth = enable_auth == "1" || enable_auth == "true" || enable_auth == "TRUE";
        }
        const std::string username = read_env_string("YUAN_SOCKS5_USER");
        const std::string password = read_env_string("YUAN_SOCKS5_PASS");
        if (!username.empty()) {
            config.server_config.username = username;
        }
        if (!password.empty()) {
            config.server_config.password = password;
        }
        config.server_config.connect_timeout_ms = static_cast<uint32_t>(
            read_env_int("YUAN_SOCKS5_CONNECT_TIMEOUT_MS", static_cast<int>(config.server_config.connect_timeout_ms)));
        config.server_config.idle_timeout_ms = static_cast<uint32_t>(
            read_env_int("YUAN_SOCKS5_IDLE_TIMEOUT_MS", static_cast<int>(config.server_config.idle_timeout_ms)));
        config.server_config.max_connections = static_cast<size_t>(
            read_env_int("YUAN_SOCKS5_MAX_CONNECTIONS", static_cast<int>(config.server_config.max_connections)));
        const std::string enable_udp = read_env_string("YUAN_SOCKS5_ENABLE_UDP_ASSOCIATE");
        if (!enable_udp.empty()) {
            config.server_config.enable_udp_associate = enable_udp == "1" || enable_udp == "true" || enable_udp == "TRUE";
        }
    }
}

int main(int argc, char **argv)
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    yuan::server::ProxyServiceConfig config;
    Socks5ServiceConfigFile socks5_config;
    std::string config_path = read_env_string("YUAN_PROXY_CONFIG");
    if (argc >= 2) {
        config_path = argv[1];
    }
    if (!load_config_file(config_path, config, socks5_config)) {
        return 1;
    }
    apply_env_overrides(config);
    apply_env_overrides(socks5_config);

    yuan::app::RuntimeContext context;
    context.app_name = "proxy-service";

    yuan::app::Application application(context);
    auto service = std::make_shared<yuan::server::ProxyService>(config);
    if (!application.add_typed_service<yuan::server::ProxyService>("proxy", service, "server.proxy", 1)) {
        std::cerr << "failed to register proxy service\n";
        return 1;
    }

    if (socks5_config.enabled) {
        auto socks5_service = std::make_shared<yuan::server::Socks5Service>(socks5_config.port, socks5_config.server_config);
        if (!application.add_typed_service<yuan::server::Socks5Service>("socks5", socks5_service, "server.socks5", 1)) {
            std::cerr << "failed to register socks5 service\n";
            return 1;
        }
    }

    yuan::app::Bootstrap bootstrap(application);
    if (!bootstrap.run()) {
        std::cerr << "failed to start proxy service\n";
        return 1;
    }

    std::cout << "proxy service listening on " << config.listen_host << ':' << config.port;
    if (!config_path.empty()) {
        std::cout << " using config " << config_path;
    }
    std::cout << '\n';
    if (socks5_config.enabled) {
        std::cout << "socks5 service listening on 0.0.0.0:" << socks5_config.port << '\n';
    }

    while (g_running.load(std::memory_order_relaxed)) {
        bootstrap.poll_workers();
        if (bootstrap.process_role() == yuan::app::ProcessRole::supervisor &&
            (bootstrap.has_failed_workers() ||
             (!bootstrap.has_running_workers() && !bootstrap.has_recovering_workers()))) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    bootstrap.shutdown();
    return 0;
}
