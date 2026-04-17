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
        size_t max_connections = 1024;
    };
}

#endif
