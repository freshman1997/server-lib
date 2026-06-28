#include "application.h"
#include "bootstrap.h"
#include "nlohmann/json.hpp"
#include "proxy_service.h"
#include "socks5/socks5_service.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>

namespace
{
    using yuan::server::Socks5ServiceConfigFile;

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

    bool read_json_int(const nlohmann::json &node, const char *key, int &out, int min_value, int max_value)
    {
        if (!node.contains(key)) {
            return false;
        }
        try {
            int value = 0;
            if (node[key].is_number_integer()) {
                value = node[key].get<int>();
            } else if (node[key].is_string()) {
                value = std::stoi(node[key].get<std::string>());
            } else {
                std::cerr << "invalid proxy config field type: " << key << '\n';
                return false;
            }
            if (value < min_value || value > max_value) {
                std::cerr << "proxy config field out of range: " << key << '=' << value << '\n';
                return false;
            }
            out = value;
            return true;
        } catch (...) {
            std::cerr << "invalid proxy config integer field: " << key << '\n';
            return false;
        }
    }

    bool read_json_bool(const nlohmann::json &node, const char *key, bool &out)
    {
        if (!node.contains(key)) {
            return false;
        }
        if (node[key].is_boolean()) {
            out = node[key].get<bool>();
            return true;
        }
        if (node[key].is_string()) {
            const std::string value = node[key].get<std::string>();
            if (value == "1" || value == "true" || value == "TRUE") {
                out = true;
                return true;
            }
            if (value == "0" || value == "false" || value == "FALSE") {
                out = false;
                return true;
            }
        }
        std::cerr << "invalid proxy config boolean field: " << key << '\n';
        return false;
    }

    bool read_json_string(const nlohmann::json &node, const char *key, std::string &out)
    {
        if (!node.contains(key)) {
            return false;
        }
        if (!node[key].is_string()) {
            std::cerr << "invalid proxy config string field: " << key << '\n';
            return false;
        }
        out = node[key].get<std::string>();
        return true;
    }

    bool read_env_int_range(const char *name, int default_value, int min_value, int max_value)
    {
        const int value = read_env_int(name, default_value);
        if (value < min_value || value > max_value) {
            std::cerr << "environment field out of range: " << name << '=' << value << ", using " << default_value << '\n';
            return default_value;
        }
        return value;
    }

    void apply_json_config(const nlohmann::json &json, yuan::server::ProxyServiceConfig &config)
    {
        const auto *proxy = &json;
        if (json.contains("proxy") && json["proxy"].is_object()) {
            proxy = &json["proxy"];
        }

        (void)read_json_string(*proxy, "listen_host", config.listen_host);
        (void)read_json_int(*proxy, "listen_port", config.port, 1, 65535);
        (void)read_json_int(*proxy, "max_active_sessions", config.max_active_sessions, 1, 10000000);
        (void)read_json_int(*proxy, "max_sessions_per_client", config.max_sessions_per_client, 0, 10000000);
        (void)read_json_int(*proxy, "header_timeout_ms", config.header_timeout_ms, 1, 24 * 60 * 60 * 1000);
        (void)read_json_int(*proxy, "idle_timeout_ms", config.idle_timeout_ms, 1, 24 * 60 * 60 * 1000);
        (void)read_json_int(*proxy, "connect_timeout_ms", config.connect_timeout_ms, 1, 24 * 60 * 60 * 1000);
        (void)read_json_int(*proxy, "drain_timeout_ms", config.drain_timeout_ms, 0, 24 * 60 * 60 * 1000);
        (void)read_json_int(*proxy, "session_snapshot_interval_ms", config.session_snapshot_interval_ms, 0, 24 * 60 * 60 * 1000);
        (void)read_json_int(*proxy, "max_header_bytes", config.max_header_bytes, 1, 100 * 1024 * 1024);
        (void)read_json_int(*proxy, "max_session_buffer_bytes", config.max_session_buffer_bytes, 1, 1024 * 1024 * 1024);
        (void)read_json_int(*proxy, "max_total_tunnel_memory", config.max_total_tunnel_memory, 0, std::numeric_limits<int>::max());
        (void)read_json_string(*proxy, "basic_auth_user", config.basic_auth_user);
        (void)read_json_string(*proxy, "basic_auth_password", config.basic_auth_password);
        if (proxy->contains("allow_targets") && (*proxy)["allow_targets"].is_array()) {
            config.allow_targets = (*proxy)["allow_targets"].get<std::vector<std::string>>();
        }
        if (proxy->contains("deny_targets") && (*proxy)["deny_targets"].is_array()) {
            config.deny_targets = (*proxy)["deny_targets"].get<std::vector<std::string>>();
        }
        (void)read_json_bool(*proxy, "allow_private_targets", config.allow_private_targets);
    }

    void apply_json_config(const nlohmann::json &json, Socks5ServiceConfigFile &config)
    {
        const auto *node = &json;
        if (json.contains("socks5") && json["socks5"].is_object()) {
            node = &json["socks5"];
        }

        (void)read_json_bool(*node, "enabled", config.enabled);
        (void)read_json_int(*node, "listen_port", config.port, 1, 65535);
        (void)read_json_bool(*node, "enable_auth", config.server_config.enable_auth);
        (void)read_json_string(*node, "username", config.server_config.username);
        (void)read_json_string(*node, "password", config.server_config.password);
        (void)read_json_bool(*node, "enable_connect", config.server_config.enable_connect);
        (void)read_json_bool(*node, "enable_bind", config.server_config.enable_bind);
        (void)read_json_bool(*node, "enable_udp_associate", config.server_config.enable_udp_associate);
        int int_value = 0;
        if (read_json_int(*node, "connect_timeout_ms", int_value, 1, 24 * 60 * 60 * 1000)) {
            config.server_config.connect_timeout_ms = static_cast<uint32_t>(int_value);
        }
        if (read_json_int(*node, "idle_timeout_ms", int_value, 1, 24 * 60 * 60 * 1000)) {
            config.server_config.idle_timeout_ms = static_cast<uint32_t>(int_value);
        }
        if (read_json_int(*node, "max_connections", int_value, 1, 10000000)) {
            config.server_config.max_connections = static_cast<size_t>(int_value);
        }
        if (read_json_int(*node, "udp_idle_timeout_ms", int_value, 1, 24 * 60 * 60 * 1000)) {
            config.server_config.udp_idle_timeout_ms = static_cast<uint32_t>(int_value);
        }
        if (read_json_int(*node, "max_datagram_size", int_value, 1, 1024 * 1024 * 1024)) {
            config.server_config.max_datagram_size = static_cast<size_t>(int_value);
        }
        if (read_json_int(*node, "max_udp_associations_per_client", int_value, 0, 10000000)) {
            config.server_config.max_udp_associations_per_client = static_cast<size_t>(int_value);
        }
        (void)read_json_string(*node, "listen_host", config.server_config.listen_host);
        (void)read_json_bool(*node, "allow_private_targets", config.server_config.allow_private_targets);
        if (node->contains("allow_targets") && (*node)["allow_targets"].is_array()) {
            config.server_config.allow_targets = (*node)["allow_targets"].get<std::vector<std::string>>();
        }
        if (node->contains("deny_targets") && (*node)["deny_targets"].is_array()) {
            config.server_config.deny_targets = (*node)["deny_targets"].get<std::vector<std::string>>();
        }
        if (read_json_int(*node, "max_sessions_per_client", int_value, 0, 10000000)) {
            config.server_config.max_sessions_per_client = static_cast<size_t>(int_value);
        }
        if (read_json_int(*node, "drain_timeout_ms", int_value, 0, 24 * 60 * 60 * 1000)) {
            config.server_config.drain_timeout_ms = static_cast<uint32_t>(int_value);
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
        config.port = read_env_int_range("YUAN_PROXY_LISTEN_PORT", config.port, 1, 65535);
        config.max_active_sessions = read_env_int_range("YUAN_PROXY_MAX_ACTIVE", config.max_active_sessions, 1, 10000000);
        config.max_sessions_per_client =
            read_env_int_range("YUAN_PROXY_MAX_SESSIONS_PER_CLIENT", config.max_sessions_per_client, 0, 10000000);
        config.header_timeout_ms = read_env_int_range("YUAN_PROXY_HEADER_TIMEOUT_MS", config.header_timeout_ms, 1, 24 * 60 * 60 * 1000);
        config.idle_timeout_ms = read_env_int_range("YUAN_PROXY_IDLE_TIMEOUT_MS", config.idle_timeout_ms, 1, 24 * 60 * 60 * 1000);
        config.connect_timeout_ms = read_env_int_range("YUAN_PROXY_CONNECT_TIMEOUT_MS", config.connect_timeout_ms, 1, 24 * 60 * 60 * 1000);
        config.drain_timeout_ms = read_env_int_range("YUAN_PROXY_DRAIN_TIMEOUT_MS", config.drain_timeout_ms, 0, 24 * 60 * 60 * 1000);
        config.session_snapshot_interval_ms =
            read_env_int_range("YUAN_PROXY_SESSION_SNAPSHOT_INTERVAL_MS", config.session_snapshot_interval_ms, 0, 24 * 60 * 60 * 1000);
        config.max_header_bytes = read_env_int_range("YUAN_PROXY_MAX_HEADER_BYTES", config.max_header_bytes, 1, 100 * 1024 * 1024);
        config.max_session_buffer_bytes = read_env_int_range("YUAN_PROXY_MAX_SESSION_BUFFER_BYTES", config.max_session_buffer_bytes, 1, 1024 * 1024 * 1024);
        config.max_total_tunnel_memory = read_env_int_range("YUAN_PROXY_MAX_TOTAL_TUNNEL_MEMORY", config.max_total_tunnel_memory, 0, std::numeric_limits<int>::max());

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

        const std::string env_allow_private = read_env_string("YUAN_PROXY_ALLOW_PRIVATE_TARGETS");
        if (!env_allow_private.empty()) {
            config.allow_private_targets = env_allow_private == "1" || env_allow_private == "true" || env_allow_private == "TRUE";
        }
    }

    void apply_env_overrides(Socks5ServiceConfigFile &config)
    {
        const std::string enabled = read_env_string("YUAN_SOCKS5_ENABLED");
        if (enabled == "0" || enabled == "false" || enabled == "FALSE") {
            config.enabled = false;
        } else if (!enabled.empty()) {
            config.enabled = true;
        }
        config.port = read_env_int_range("YUAN_SOCKS5_PORT", config.port, 1, 65535);
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
            read_env_int_range("YUAN_SOCKS5_CONNECT_TIMEOUT_MS", static_cast<int>(config.server_config.connect_timeout_ms), 1, 24 * 60 * 60 * 1000));
        config.server_config.idle_timeout_ms = static_cast<uint32_t>(
            read_env_int_range("YUAN_SOCKS5_IDLE_TIMEOUT_MS", static_cast<int>(config.server_config.idle_timeout_ms), 1, 24 * 60 * 60 * 1000));
        config.server_config.max_connections = static_cast<size_t>(
            read_env_int_range("YUAN_SOCKS5_MAX_CONNECTIONS", static_cast<int>(config.server_config.max_connections), 1, 10000000));
        const std::string enable_udp = read_env_string("YUAN_SOCKS5_ENABLE_UDP_ASSOCIATE");
        if (!enable_udp.empty()) {
            config.server_config.enable_udp_associate = enable_udp == "1" || enable_udp == "true" || enable_udp == "TRUE";
        }
        config.server_config.udp_idle_timeout_ms = static_cast<uint32_t>(
            read_env_int_range("YUAN_SOCKS5_UDP_IDLE_TIMEOUT_MS", static_cast<int>(config.server_config.udp_idle_timeout_ms), 1, 24 * 60 * 60 * 1000));
        config.server_config.max_datagram_size = static_cast<size_t>(
            read_env_int_range("YUAN_SOCKS5_MAX_DATAGRAM_SIZE", static_cast<int>(config.server_config.max_datagram_size), 1, 1024 * 1024 * 1024));
        config.server_config.max_udp_associations_per_client = static_cast<size_t>(
            read_env_int_range("YUAN_SOCKS5_MAX_UDP_PER_CLIENT", static_cast<int>(config.server_config.max_udp_associations_per_client), 0, 10000000));

        const std::string socks5_listen_host = read_env_string("YUAN_SOCKS5_LISTEN_HOST");
        if (!socks5_listen_host.empty()) {
            config.server_config.listen_host = socks5_listen_host;
        }
        const std::string socks5_allow_private = read_env_string("YUAN_SOCKS5_ALLOW_PRIVATE_TARGETS");
        if (!socks5_allow_private.empty()) {
            config.server_config.allow_private_targets = socks5_allow_private == "1" || socks5_allow_private == "true" || socks5_allow_private == "TRUE";
        }
        const std::string socks5_allow_targets = read_env_string("YUAN_SOCKS5_ALLOW_TARGETS");
        if (!socks5_allow_targets.empty()) {
            config.server_config.allow_targets = split_csv(socks5_allow_targets);
        }
        const std::string socks5_deny_targets = read_env_string("YUAN_SOCKS5_DENY_TARGETS");
        if (!socks5_deny_targets.empty()) {
            config.server_config.deny_targets = split_csv(socks5_deny_targets);
        }
        config.server_config.max_sessions_per_client = static_cast<size_t>(
            read_env_int_range("YUAN_SOCKS5_MAX_SESSIONS_PER_CLIENT", static_cast<int>(config.server_config.max_sessions_per_client), 0, 10000000));
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
        std::cout << "socks5 service listening on " << socks5_config.server_config.listen_host << ':' << socks5_config.port << '\n';
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
