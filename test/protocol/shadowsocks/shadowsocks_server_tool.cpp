#include "shadowsocks.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace yuan::net::shadowsocks;

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

    std::string read_env_str(const char *name, const std::string &default_value)
    {
        const char *raw = std::getenv(name);
        if (!raw) {
            return default_value;
        }
        return raw;
    }

    bool read_env_bool(const char *name, bool default_value)
    {
        const std::string value = read_env_str(name, default_value ? "1" : "0");
        return value == "1" || value == "true" || value == "TRUE";
    }
}

int main()
{
    ShadowsocksServerConfig config;
    config.listen_host = read_env_str("YUAN_SS_LISTEN_HOST", "127.0.0.1");
    config.port = read_env_int("YUAN_SS_PORT", 8388);
    config.method = read_env_str("YUAN_SS_METHOD", "chacha20-ietf-poly1305");
    config.password = read_env_str("YUAN_SS_PASSWORD", "secret");
    config.enable_tcp = read_env_bool("YUAN_SS_ENABLE_TCP", true);
    config.enable_udp = read_env_bool("YUAN_SS_ENABLE_UDP", true);
    config.allow_private_targets = read_env_bool("YUAN_SS_ALLOW_PRIVATE_TARGETS", true);
    config.connect_timeout_ms = static_cast<uint32_t>(read_env_int("YUAN_SS_CONNECT_TIMEOUT_MS", 5000));
    config.idle_timeout_ms = static_cast<uint32_t>(read_env_int("YUAN_SS_IDLE_TIMEOUT_MS", 120000));
    config.udp_idle_timeout_ms = static_cast<uint32_t>(read_env_int("YUAN_SS_UDP_IDLE_TIMEOUT_MS", 120000));

    std::cout << "[shadowsocks_server_tool] host=" << config.listen_host
              << " port=" << config.port
              << " method=" << config.method
              << " tcp=" << (config.enable_tcp ? "on" : "off")
              << " udp=" << (config.enable_udp ? "on" : "off")
              << std::endl;

    auto server = std::make_shared<ShadowsocksServer>(config);
    if (!server->init(config.listen_host, config.port)) {
        std::cerr << "[shadowsocks_server_tool] init failed" << std::endl;
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::thread server_thread([server]() {
        server->serve();
    });

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server->stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    std::cout << "[shadowsocks_server_tool] stopped" << std::endl;
    return 0;
}
