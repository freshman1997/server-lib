#include "websocket_utils.h"
#include "base/utils/base64.h"
#include "base/utils/string_util.h"
#include "openssl/sha.h"
#include <array>
#include <cstdint>
#include <random>
#include <span>
#include <string>

namespace yuan::net::websocket
{
    std::vector<uint8_t> WebSocketUtils::gen_mask_keys()
    {
        static thread_local std::mt19937_64 generator(std::random_device {}());
        static thread_local std::uniform_int_distribution<uint32_t> distribution;
        uint32_t val = distribution(generator);
        return {
            static_cast<uint8_t>((val >> 24) & 0xff),
            static_cast<uint8_t>((val >> 16) & 0xff),
            static_cast<uint8_t>((val >> 8) & 0xff),
            static_cast<uint8_t>(val & 0xff),
        };
    }

    std::string WebSocketUtils::generate_server_key(const std::string & clientKey)
    {
        std::string magic = clientKey + magic_string_.data();
        unsigned char *hash = SHA1((unsigned char *)magic.c_str(), magic.size(), nullptr);
        return base::util::base64_encode(std::span<const std::uint8_t>(hash, SHA_DIGEST_LENGTH));
    }

    std::string WebSocketUtils::gen_magic_string()
    {
        static thread_local std::mt19937_64 generator(std::random_device {}());
        static thread_local std::uniform_int_distribution<uint32_t> distribution;
        std::array<std::uint8_t, 16> nonce{};
        for (std::size_t i = 0; i < nonce.size(); i += 4) {
            const uint32_t value = distribution(generator);
            nonce[i] = static_cast<std::uint8_t>((value >> 24) & 0xff);
            nonce[i + 1] = static_cast<std::uint8_t>((value >> 16) & 0xff);
            nonce[i + 2] = static_cast<std::uint8_t>((value >> 8) & 0xff);
            nonce[i + 3] = static_cast<std::uint8_t>(value & 0xff);
        }
        return base::util::base64_encode(std::span<const std::uint8_t>(nonce.data(), nonce.size()));
    }

    std::string_view WebSocketUtils::magic_string_ = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
}
