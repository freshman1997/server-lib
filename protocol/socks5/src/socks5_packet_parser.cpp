#include "socks5_packet_parser.h"
#include "buffer/byte_buffer.h"
#include "endian/endian.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <cstring>

namespace yuan::net::socks5
{
    std::optional<Socks5Greeting> Socks5PacketParser::parse_greeting(const ::yuan::buffer::ByteBuffer & buf)
    {
        auto span = buf.readable_span();
        if (span.size() < 2) {
            return std::nullopt;
        }

        const uint8_t *data = span.data();
        if (data[0] != static_cast<uint8_t>(SocksVersion::v5)) {
            return std::nullopt;
        }

        uint8_t method_count = data[1];
        if (span.size() < static_cast<size_t>(2 + method_count)) {
            return std::nullopt;
        }

        Socks5Greeting greeting;
        greeting.version = data[0];
        greeting.method_count = method_count;
        std::memcpy(greeting.methods, data + 2, method_count);
        return greeting;
    }

    std::optional<std::pair<std::string, std::string> > Socks5PacketParser::parse_auth_request(const ::yuan::buffer::ByteBuffer & buf)
    {
        auto span = buf.readable_span();
        if (span.size() < 2) {
            return std::nullopt;
        }

        const uint8_t *data = span.data();
        uint8_t ulen = data[0];
        if (span.size() < static_cast<size_t>(1 + ulen + 1)) {
            return std::nullopt;
        }

        std::string username(reinterpret_cast<const char *>(data + 1), ulen);
        uint8_t plen = data[1 + ulen];
        if (span.size() < static_cast<size_t>(1 + ulen + 1 + plen)) {
            return std::nullopt;
        }

        std::string password(reinterpret_cast<const char *>(data + 1 + ulen + 1), plen);
        return std::make_pair(std::move(username), std::move(password));
    }

    size_t Socks5PacketParser::address_length(AddressType atyp, const uint8_t * data)
    {
        switch (atyp) {
        case AddressType::ipv4:
            return 4;
        case AddressType::domain:
            return 1 + data[0];
        case AddressType::ipv6:
            return 16;
        default:
            return 0;
        }
    }

    std::string Socks5PacketParser::read_address(const uint8_t * data, AddressType atyp, size_t & offset)
    {
        std::string addr;
        switch (atyp) {
        case AddressType::ipv4: {
            char str[INET_ADDRSTRLEN];
            struct in_addr in;
            std::memcpy(&in, data + offset, 4);
            inet_ntop(AF_INET, &in, str, sizeof(str));
            addr = str;
            offset += 4;
            break;
        }
        case AddressType::domain: {
            uint8_t len = data[offset];
            offset += 1;
            addr.assign(reinterpret_cast<const char *>(data + offset), len);
            offset += len;
            break;
        }
        case AddressType::ipv6: {
            char str[INET6_ADDRSTRLEN];
            struct in6_addr in6;
            std::memcpy(&in6, data + offset, 16);
            inet_ntop(AF_INET6, &in6, str, sizeof(str));
            addr = str;
            offset += 16;
            break;
        }
        default:
            break;
        }
        return addr;
    }

    std::optional<Socks5Request> Socks5PacketParser::parse_request(const ::yuan::buffer::ByteBuffer & buf)
    {
        auto span = buf.readable_span();
        if (span.size() < 4) {
            return std::nullopt;
        }

        const uint8_t *data = span.data();
        if (data[0] != static_cast<uint8_t>(SocksVersion::v5)) {
            return std::nullopt;
        }

        Socks5Request req;
        req.version = data[0];
        req.cmd = static_cast<Command>(data[1]);
        req.reserved = data[2];
        req.atyp = static_cast<AddressType>(data[3]);

        size_t addr_len = address_length(req.atyp, data + 4);
        size_t total_needed = 4 + addr_len + 2;
        if (span.size() < total_needed) {
            return std::nullopt;
        }

        size_t offset = 4;
        std::string addr = read_address(data, req.atyp, offset);
        std::memcpy(req.address, addr.data(), addr.size());
        req.address[addr.size()] = '\0';

        uint16_t port = 0;
        std::memcpy(&port, data + offset, 2);
        req.port = yuan::endian::networkToHost16(port);

        return req;
    }

    ::yuan::buffer::ByteBuffer Socks5PacketParser::build_method_select_reply(AuthMethod method)
    {
        ::yuan::buffer::ByteBuffer buf(2);
        buf.append_u8(static_cast<uint8_t>(SocksVersion::v5));
        buf.append_u8(static_cast<uint8_t>(method));
        return buf;
    }

    ::yuan::buffer::ByteBuffer Socks5PacketParser::build_auth_reply(bool success)
    {
        ::yuan::buffer::ByteBuffer buf(2);
        buf.append_u8(static_cast<uint8_t>(SocksVersion::v5));
        buf.append_u8(success ? 0x00 : 0x01);
        return buf;
    }

    ::yuan::buffer::ByteBuffer Socks5PacketParser::build_reply(
        ReplyCode reply,
        AddressType atyp,
        const std::string & bind_addr,
        uint16_t bind_port)
    {
        ::yuan::buffer::ByteBuffer buf(64);
        buf.append_u8(static_cast<uint8_t>(SocksVersion::v5));
        buf.append_u8(static_cast<uint8_t>(reply));
        buf.append_u8(static_cast<uint8_t>(0x00));
        buf.append_u8(static_cast<uint8_t>(atyp));

        switch (atyp) {
        case AddressType::ipv4: {
            struct in_addr in;
            inet_pton(AF_INET, bind_addr.c_str(), &in);
            buf.append(reinterpret_cast<const uint8_t *>(&in), 4);
            break;
        }
        case AddressType::domain: {
            uint8_t len = static_cast<uint8_t>(bind_addr.size());
            buf.append_u8(len);
            buf.append(reinterpret_cast<const uint8_t *>(bind_addr.data()), len);
            break;
        }
        case AddressType::ipv6: {
            struct in6_addr in6;
            inet_pton(AF_INET6, bind_addr.c_str(), &in6);
            buf.append(reinterpret_cast<const uint8_t *>(&in6), 16);
            break;
        }
        default:
            break;
        }

        uint16_t net_port = yuan::endian::hostToNetwork16(bind_port);
        buf.append(reinterpret_cast<const uint8_t *>(&net_port), 2);
        return buf;
    }

    std::optional<Socks5UdpHeader> Socks5PacketParser::parse_udp_header(const ::yuan::buffer::ByteBuffer & buf)
    {
        auto span = buf.readable_span();
        if (span.size() < 4) {
            return std::nullopt;
        }

        const uint8_t *data = span.data();

        Socks5UdpHeader header;
        uint16_t reserved = 0;
        std::memcpy(&reserved, data, 2);
        header.reserved = yuan::endian::networkToHost16(reserved);
        header.fragment = data[2];
        header.atyp = static_cast<AddressType>(data[3]);

        size_t addr_len = address_length(header.atyp, data + 4);
        size_t total_needed = 4 + addr_len + 2;
        if (span.size() < total_needed) {
            return std::nullopt;
        }

        size_t offset = 4;
        std::string addr = read_address(data, header.atyp, offset);
        std::memcpy(header.address, addr.data(), addr.size());
        header.address[addr.size()] = '\0';

        uint16_t port = 0;
        std::memcpy(&port, data + offset, 2);
        header.port = yuan::endian::networkToHost16(port);

        return header;
    }

    ::yuan::buffer::ByteBuffer Socks5PacketParser::build_udp_header(
        AddressType atyp,
        const std::string & address,
        uint16_t port)
    {
        ::yuan::buffer::ByteBuffer buf(64);
        uint16_t reserved = 0;
        uint16_t net_reserved = yuan::endian::hostToNetwork16(reserved);
        buf.append(reinterpret_cast<const uint8_t *>(&net_reserved), 2);
        buf.append_u8(static_cast<uint8_t>(0x00));
        buf.append_u8(static_cast<uint8_t>(atyp));

        switch (atyp) {
        case AddressType::ipv4: {
            struct in_addr in;
            inet_pton(AF_INET, address.c_str(), &in);
            buf.append(reinterpret_cast<const uint8_t *>(&in), 4);
            break;
        }
        case AddressType::domain: {
            uint8_t len = static_cast<uint8_t>(address.size());
            buf.append_u8(len);
            buf.append(reinterpret_cast<const uint8_t *>(address.data()), len);
            break;
        }
        case AddressType::ipv6: {
            struct in6_addr in6;
            inet_pton(AF_INET6, address.c_str(), &in6);
            buf.append(reinterpret_cast<const uint8_t *>(&in6), 16);
            break;
        }
        default:
            break;
        }

        uint16_t net_port = yuan::endian::hostToNetwork16(port);
        buf.append(reinterpret_cast<const uint8_t *>(&net_port), 2);
        return buf;
    }
}
