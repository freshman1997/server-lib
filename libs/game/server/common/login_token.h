#ifndef YUAN_GAME_SERVER_COMMON_LOGIN_TOKEN_H
#define YUAN_GAME_SERVER_COMMON_LOGIN_TOKEN_H

#include "common/service_node.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace yuan::game::server
{
    using LoginTokenId = std::string;

    inline constexpr std::uint64_t kDefaultLoginTokenSecret = 0x9e3779b97f4a7c15ULL;
    inline std::string login_token_mac(PackedGameServiceId zone_service_id, std::uint64_t expires_at_ms, std::uint64_t secret = kDefaultLoginTokenSecret)
    {
        const auto key = std::to_string(secret);
        const auto data = std::to_string(zone_service_id) + "." + std::to_string(expires_at_ms);
        unsigned int digest_len = 0;
        unsigned char digest[EVP_MAX_MD_SIZE]{};
        if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(data.data()), data.size(), digest, &digest_len)) {
            return {};
        }
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < digest_len; ++i) {
            out << std::setw(2) << static_cast<unsigned int>(digest[i]);
        }
        return out.str();
    }

    inline LoginTokenId encode_login_token_id(PackedGameServiceId zone_service_id, std::uint64_t expires_at_ms, std::uint64_t secret = kDefaultLoginTokenSecret)
    {
        return std::to_string(zone_service_id) + "." + std::to_string(expires_at_ms) + "." + login_token_mac(zone_service_id, expires_at_ms, secret);
    }

    inline std::optional<PackedGameServiceId> decode_login_token_id(const LoginTokenId &token_id, std::uint64_t now_ms, std::uint64_t secret = kDefaultLoginTokenSecret)
    {
        const auto first_separator = token_id.find('.');
        const auto second_separator = first_separator == std::string::npos ? std::string::npos : token_id.find('.', first_separator + 1);
        if (first_separator == std::string::npos || second_separator == std::string::npos || first_separator == 0 || second_separator + 1 >= token_id.size()) {
            return std::nullopt;
        }

        try {
            const auto zone_service_id = static_cast<PackedGameServiceId>(std::stoull(token_id.substr(0, first_separator)));
            const auto expires_at_ms = static_cast<std::uint64_t>(std::stoull(token_id.substr(first_separator + 1, second_separator - first_separator - 1)));
            const auto mac = token_id.substr(second_separator + 1);
            if (zone_service_id == 0 || expires_at_ms == 0 || expires_at_ms <= now_ms || mac != login_token_mac(zone_service_id, expires_at_ms, secret)) {
                return std::nullopt;
            }
            return zone_service_id;
        } catch (...) {
            return std::nullopt;
        }
    }

}

#endif
