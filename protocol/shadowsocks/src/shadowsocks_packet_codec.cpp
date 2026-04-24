#include "shadowsocks_packet_codec.h"

#include "endian/endian.hpp"
#include "shadowsocks_crypto.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <cstring>

namespace yuan::net::shadowsocks
{
    bool ShadowsocksPacketCodec::append_target_address(::yuan::buffer::ByteBuffer &buf, const TargetAddress &target)
    {
        buf.append_u8(static_cast<uint8_t>(target.atyp));

        switch (target.atyp) {
        case AddressType::ipv4: {
            in_addr in{};
            if (inet_pton(AF_INET, target.host.c_str(), &in) != 1) {
                return false;
            }
            buf.append(reinterpret_cast<const uint8_t *>(&in), 4);
            break;
        }
        case AddressType::ipv6: {
            in6_addr in6{};
            if (inet_pton(AF_INET6, target.host.c_str(), &in6) != 1) {
                return false;
            }
            buf.append(reinterpret_cast<const uint8_t *>(&in6), 16);
            break;
        }
        case AddressType::domain: {
            if (target.host.size() > 255) {
                return false;
            }
            buf.append_u8(static_cast<uint8_t>(target.host.size()));
            buf.append(reinterpret_cast<const uint8_t *>(target.host.data()), target.host.size());
            break;
        }
        }

        const auto net_port = ::yuan::endian::hostToNetwork16(target.port);
        buf.append(reinterpret_cast<const uint8_t *>(&net_port), sizeof(net_port));
        return true;
    }

    std::optional<TargetAddress> ShadowsocksPacketCodec::parse_target_address(const ::yuan::buffer::ByteBuffer &buf,
                                                                               std::size_t &consumed_bytes)
    {
        auto span = buf.readable_span();
        return parse_target_address(reinterpret_cast<const uint8_t *>(span.data()), span.size(), consumed_bytes);
    }

    std::optional<TargetAddress> ShadowsocksPacketCodec::parse_target_address(const uint8_t *data,
                                                                               std::size_t size,
                                                                               std::size_t &consumed_bytes)
    {
        consumed_bytes = 0;
        if (data == nullptr || size < 1) {
            return std::nullopt;
        }

        TargetAddress target;
        target.atyp = static_cast<AddressType>(data[0]);
        std::size_t offset = 1;

        switch (target.atyp) {
        case AddressType::ipv4: {
            if (size < offset + 4 + 2) {
                return std::nullopt;
            }
            char out[INET_ADDRSTRLEN] = {0};
            in_addr in{};
            std::memcpy(&in, data + offset, 4);
            if (inet_ntop(AF_INET, &in, out, sizeof(out)) == nullptr) {
                return std::nullopt;
            }
            target.host = out;
            offset += 4;
            break;
        }
        case AddressType::ipv6: {
            if (size < offset + 16 + 2) {
                return std::nullopt;
            }
            char out[INET6_ADDRSTRLEN] = {0};
            in6_addr in6{};
            std::memcpy(&in6, data + offset, 16);
            if (inet_ntop(AF_INET6, &in6, out, sizeof(out)) == nullptr) {
                return std::nullopt;
            }
            target.host = out;
            offset += 16;
            break;
        }
        case AddressType::domain: {
            if (size < offset + 1) {
                return std::nullopt;
            }
            const auto domain_len = static_cast<std::size_t>(data[offset]);
            offset += 1;
            if (domain_len == 0 || size < offset + domain_len + 2) {
                return std::nullopt;
            }
            target.host.assign(reinterpret_cast<const char *>(data + offset), domain_len);
            offset += domain_len;
            break;
        }
        default:
            return std::nullopt;
        }

        uint16_t net_port = 0;
        std::memcpy(&net_port, data + offset, sizeof(net_port));
        target.port = ::yuan::endian::networkToHost16(net_port);
        offset += sizeof(net_port);

        consumed_bytes = offset;
        return target;
    }

    bool ShadowsocksPacketCodec::append_tcp_chunk(::yuan::buffer::ByteBuffer &out,
                                                   CipherMethod method,
                                                   const std::vector<uint8_t> &subkey,
                                                   std::vector<uint8_t> &send_nonce,
                                                   const uint8_t *payload,
                                                   std::size_t payload_size)
    {
        if (payload == nullptr && payload_size > 0) {
            return false;
        }

        if (payload_size > 0x3FFF) {
            return false;
        }

        const auto &spec = method_spec(method);
        if (send_nonce.size() != spec.nonce_size) {
            return false;
        }

        const uint16_t payload_len = static_cast<uint16_t>(payload_size);
        uint8_t len_plain[2] = {
            static_cast<uint8_t>((payload_len >> 8) & 0x3F),
            static_cast<uint8_t>(payload_len & 0xFF)
        };

        std::vector<uint8_t> encrypted_len;
        if (!ShadowsocksCrypto::aead_encrypt(method,
                                             subkey,
                                             send_nonce,
                                             len_plain,
                                             sizeof(len_plain),
                                             nullptr,
                                             0,
                                             encrypted_len)) {
            return false;
        }

        if (!ShadowsocksCrypto::increment_nonce(send_nonce)) {
            return false;
        }

        std::vector<uint8_t> encrypted_payload;
        if (!ShadowsocksCrypto::aead_encrypt(method,
                                             subkey,
                                             send_nonce,
                                             payload,
                                             payload_size,
                                             nullptr,
                                             0,
                                             encrypted_payload)) {
            return false;
        }

        if (!ShadowsocksCrypto::increment_nonce(send_nonce)) {
            return false;
        }

        out.append(reinterpret_cast<const char *>(encrypted_len.data()), encrypted_len.size());
        out.append(reinterpret_cast<const char *>(encrypted_payload.data()), encrypted_payload.size());
        return true;
    }

    ShadowsocksPacketCodec::AeadChunkParseResult ShadowsocksPacketCodec::try_parse_tcp_chunk(
        const uint8_t *data,
        std::size_t size,
        CipherMethod method,
        const std::vector<uint8_t> &subkey,
        std::vector<uint8_t> &recv_nonce)
    {
        AeadChunkParseResult result;

        const auto &spec = method_spec(method);
        if (data == nullptr || recv_nonce.size() != spec.nonce_size || subkey.size() != spec.key_size) {
            result.malformed = true;
            return result;
        }

        const std::size_t len_block_size = 2 + spec.tag_size;
        if (size < len_block_size) {
            return result;
        }

        std::vector<uint8_t> len_plain;
        if (!ShadowsocksCrypto::aead_decrypt(method,
                                             subkey,
                                             recv_nonce,
                                             data,
                                             len_block_size,
                                             nullptr,
                                             0,
                                             len_plain)) {
            result.malformed = true;
            return result;
        }

        if (len_plain.size() != 2) {
            result.malformed = true;
            return result;
        }

        const uint16_t payload_len = static_cast<uint16_t>((static_cast<uint16_t>(len_plain[0]) << 8) |
                                                           static_cast<uint16_t>(len_plain[1]));
        if ((payload_len & 0xC000) != 0) {
            result.malformed = true;
            return result;
        }

        const std::size_t payload_size = static_cast<std::size_t>(payload_len);
        const std::size_t payload_block_size = payload_size + spec.tag_size;
        if (size < len_block_size + payload_block_size) {
            return result;
        }

        std::vector<uint8_t> payload_plain;
        std::vector<uint8_t> nonce_next = recv_nonce;
        if (!ShadowsocksCrypto::increment_nonce(nonce_next)) {
            result.malformed = true;
            return result;
        }

        if (!ShadowsocksCrypto::aead_decrypt(method,
                                             subkey,
                                             nonce_next,
                                             data + len_block_size,
                                             payload_block_size,
                                             nullptr,
                                             0,
                                             payload_plain)) {
            result.malformed = true;
            return result;
        }

        if (payload_plain.size() != payload_size) {
            result.malformed = true;
            return result;
        }

        if (!ShadowsocksCrypto::increment_nonce(nonce_next)) {
            result.malformed = true;
            return result;
        }

        recv_nonce = std::move(nonce_next);
        result.complete = true;
        result.consumed = len_block_size + payload_block_size;
        result.plaintext = std::move(payload_plain);
        return result;
    }

    bool ShadowsocksPacketCodec::append_udp_packet(::yuan::buffer::ByteBuffer &out,
                                                   CipherMethod method,
                                                   const std::vector<uint8_t> &master_key,
                                                   const uint8_t *payload,
                                                   std::size_t payload_size)
    {
        if ((payload == nullptr && payload_size > 0) || master_key.empty()) {
            return false;
        }

        const auto &spec = method_spec(method);
        if (master_key.size() != spec.key_size) {
            return false;
        }

        std::vector<uint8_t> salt;
        if (!ShadowsocksCrypto::random_bytes(spec.salt_size, salt)) {
            return false;
        }

        std::vector<uint8_t> subkey;
        if (!ShadowsocksCrypto::derive_subkey(master_key,
                                              method,
                                              salt.data(),
                                              salt.size(),
                                              subkey)) {
            return false;
        }

        std::vector<uint8_t> nonce(spec.nonce_size, 0);
        std::vector<uint8_t> cipher;
        if (!ShadowsocksCrypto::aead_encrypt(method,
                                             subkey,
                                             nonce,
                                             payload,
                                             payload_size,
                                             nullptr,
                                             0,
                                             cipher)) {
            return false;
        }

        out.append(reinterpret_cast<const char *>(salt.data()), salt.size());
        out.append(reinterpret_cast<const char *>(cipher.data()), cipher.size());
        return true;
    }

    ShadowsocksPacketCodec::UdpPacketParseResult ShadowsocksPacketCodec::parse_udp_packet(
        const uint8_t *data,
        std::size_t size,
        CipherMethod method,
        const std::vector<uint8_t> &master_key)
    {
        UdpPacketParseResult result;

        const auto &spec = method_spec(method);
        if (data == nullptr ||
            master_key.size() != spec.key_size ||
            size < spec.salt_size + spec.tag_size) {
            result.malformed = true;
            return result;
        }

        const uint8_t *salt = data;
        const uint8_t *cipher = data + spec.salt_size;
        const std::size_t cipher_size = size - spec.salt_size;

        std::vector<uint8_t> subkey;
        if (!ShadowsocksCrypto::derive_subkey(master_key,
                                              method,
                                              salt,
                                              spec.salt_size,
                                              subkey)) {
            result.malformed = true;
            return result;
        }

        std::vector<uint8_t> nonce(spec.nonce_size, 0);
        std::vector<uint8_t> plain;
        if (!ShadowsocksCrypto::aead_decrypt(method,
                                             subkey,
                                             nonce,
                                             cipher,
                                             cipher_size,
                                             nullptr,
                                             0,
                                             plain)) {
            result.malformed = true;
            return result;
        }

        result.complete = true;
        result.plaintext = std::move(plain);
        return result;
    }
}
