#ifndef __NET_SOCKS5_SOCKS5_CONFIG_H__
#define __NET_SOCKS5_SOCKS5_CONFIG_H__

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace yuan::net::socks5
{
    struct Socks5ServerConfig
    {
        bool enable_auth = false;
        std::string username;
        std::string password;
        bool enable_connect = true;
        bool enable_bind = false;
        bool enable_udp_associate = false;
        uint32_t connect_timeout_ms = 10000;
        uint32_t idle_timeout_ms = 300000;
        size_t max_connections = 8192;
        uint32_t udp_idle_timeout_ms = 300000;
        size_t max_datagram_size = 65535;
        size_t max_udp_associations_per_client = 0;
        std::string listen_host = "0.0.0.0";
        bool allow_private_targets = false;
        std::vector<std::string> allow_targets;
        std::vector<std::string> deny_targets;
        size_t max_sessions_per_client = 0;
        uint32_t drain_timeout_ms = 5000;
    };
}

#endif
