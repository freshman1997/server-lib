#ifndef __NET_SOCKS5_SOCKS5_PROTOCOL_H__
#define __NET_SOCKS5_SOCKS5_PROTOCOL_H__

#include <cstdint>

namespace yuan::net::socks5
{
    enum class SocksVersion : uint8_t {
        v5 = 0x05
    };

    enum class AuthMethod : uint8_t {
        no_auth = 0x00,
        gssapi = 0x01,
        username_password = 0x02,
        no_acceptable = 0xFF
    };

    enum class Command : uint8_t {
        connect = 0x01,
        bind = 0x02,
        udp_associate = 0x03
    };

    enum class AddressType : uint8_t {
        ipv4 = 0x01,
        domain = 0x03,
        ipv6 = 0x04
    };

    enum class ReplyCode : uint8_t {
        succeeded = 0x00,
        general_failure = 0x01,
        connection_not_allowed = 0x02,
        network_unreachable = 0x03,
        host_unreachable = 0x04,
        connection_refused = 0x05,
        ttl_expired = 0x06,
        command_not_supported = 0x07,
        address_type_not_supported = 0x08
    };

    struct Socks5Greeting
    {
        uint8_t version;
        uint8_t method_count;
        uint8_t methods[255];
    };

    struct Socks5Request
    {
        uint8_t version;
        Command cmd;
        uint8_t reserved;
        AddressType atyp;
        char address[256];
        uint16_t port;
    };

    struct Socks5Reply
    {
        uint8_t version;
        ReplyCode reply;
        uint8_t reserved;
        AddressType atyp;
        char address[256];
        uint16_t port;
    };

    struct Socks5UdpHeader
    {
        uint16_t reserved;
        uint8_t fragment;
        AddressType atyp;
        char address[256];
        uint16_t port;
    };
}

#endif
