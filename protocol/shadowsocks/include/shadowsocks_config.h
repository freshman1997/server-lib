#ifndef __NET_SHADOWSOCKS_SHADOWSOCKS_CONFIG_H__
#define __NET_SHADOWSOCKS_SHADOWSOCKS_CONFIG_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::shadowsocks
{
    struct ShadowsocksServerConfig
    {
        std::string listen_host = "0.0.0.0";
        int port = 8388;

        std::string method = "chacha20-ietf-poly1305";
        std::string password;

        bool enable_tcp = true;
        bool enable_udp = true;

        uint32_t connect_timeout_ms = 10000;
        uint32_t idle_timeout_ms = 300000;
        uint32_t udp_idle_timeout_ms = 300000;

        std::size_t max_connections = 8192;
        std::size_t max_sessions_per_client = 0;
        std::size_t max_datagram_size = 65535;

        bool allow_private_targets = false;
        std::vector<std::string> allow_targets;
        std::vector<std::string> deny_targets;
    };
}

#endif
