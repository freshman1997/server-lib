#ifndef __NET_SOCKS5_SOCKS5_PACKET_PARSER_H__
#define __NET_SOCKS5_SOCKS5_PACKET_PARSER_H__

#include "socks5_protocol.h"
#include "buffer/byte_buffer.h"

#include <cstdint>
#include <optional>
#include <string>

namespace yuan::net::socks5
{
    class Socks5PacketParser
    {
    public:
        static std::optional<Socks5Greeting> parse_greeting(const ::yuan::buffer::ByteBuffer &buf);

        static std::optional<std::pair<std::string, std::string> > parse_auth_request(const ::yuan::buffer::ByteBuffer &buf);

        static std::optional<Socks5Request> parse_request(const ::yuan::buffer::ByteBuffer &buf);

        static ::yuan::buffer::ByteBuffer build_method_select_reply(AuthMethod method);

        static ::yuan::buffer::ByteBuffer build_auth_reply(bool success);

        static ::yuan::buffer::ByteBuffer build_reply(
            ReplyCode reply,
            AddressType atyp = AddressType::ipv4,
            const std::string &bind_addr = "0.0.0.0",
            uint16_t bind_port = 0);

        static std::optional<Socks5UdpHeader> parse_udp_header(const ::yuan::buffer::ByteBuffer &buf);

        static ::yuan::buffer::ByteBuffer build_udp_header(
            AddressType atyp,
            const std::string &address,
            uint16_t port);

    private:
        static std::string read_address(const uint8_t *data, AddressType atyp, size_t &offset);
        static size_t address_length(AddressType atyp, const uint8_t *data);
    };
}

#endif
