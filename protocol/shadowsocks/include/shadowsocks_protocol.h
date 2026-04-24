#ifndef __NET_SHADOWSOCKS_SHADOWSOCKS_PROTOCOL_H__
#define __NET_SHADOWSOCKS_SHADOWSOCKS_PROTOCOL_H__

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace yuan::net::shadowsocks
{
    enum class AddressType : uint8_t {
        ipv4 = 0x01,
        domain = 0x03,
        ipv6 = 0x04
    };

    enum class CipherMethod {
        aes_128_gcm,
        aes_256_gcm,
        chacha20_ietf_poly1305
    };

    struct MethodSpec
    {
        CipherMethod method;
        const char *name = nullptr;
        std::size_t key_size = 0;
        std::size_t salt_size = 0;
        std::size_t nonce_size = 0;
        std::size_t tag_size = 0;
    };

    struct TargetAddress
    {
        AddressType atyp = AddressType::domain;
        std::string host;
        uint16_t port = 0;
    };

    std::optional<CipherMethod> parse_method(std::string_view method_name);
    const MethodSpec &method_spec(CipherMethod method);

    bool is_supported_method(std::string_view method_name);
}

#endif
