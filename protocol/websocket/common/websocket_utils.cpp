#include "websocket_utils.h"
#include "base/utils/base64.h"
#include "base/utils/string_util.h"
#include "openssl/sha.h"
#include <random>
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
        return base::util::base64_encode((const char *)hash);
    }

    std::string WebSocketUtils::gen_magic_string()
    {
        static thread_local std::mt19937_64 generator(std::random_device {}());
        static thread_local std::uniform_int_distribution<uint32_t> distribution;
        const std::string &res = base::util::to_hex_string(distribution(generator) ^ 0x12fdefacd) + "-" + base::util::to_hex_string(distribution(generator) ^ 0x12fdeface) + "-" + base::util::to_hex_string(distribution(generator) ^ 0x12fdefaef) + "-" + base::util::to_hex_string(distribution(generator));
        return base::util::base64_encode(res);
    }

    std::string_view WebSocketUtils::magic_string_ = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
}