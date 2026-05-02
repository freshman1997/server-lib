#include "nas/nas_auth_service.h"

#include "base/utils/base64.h"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

namespace yuan::server::nas
{
    namespace
    {
        std::uint64_t fnv1a64(std::string_view text)
        {
            std::uint64_t hash = 1469598103934665603ull;
            for (unsigned char ch : text) {
                hash ^= ch;
                hash *= 1099511628211ull;
            }
            return hash;
        }

        bool constant_time_equal(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size()) {
                return false;
            }
            unsigned char diff = 0;
            for (std::size_t i = 0; i < lhs.size(); ++i) {
                diff |= static_cast<unsigned char>(lhs[i] ^ rhs[i]);
            }
            return diff == 0;
        }

        bool starts_with_ci(std::string_view text, std::string_view prefix)
        {
            if (text.size() < prefix.size()) {
                return false;
            }
            for (std::size_t i = 0; i < prefix.size(); ++i) {
                const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
                const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
                if (a != b) {
                    return false;
                }
            }
            return true;
        }

        std::uint32_t rotl32(std::uint32_t value, std::uint32_t bits)
        {
            return (value << bits) | (value >> (32 - bits));
        }

        std::uint32_t md4_f(std::uint32_t x, std::uint32_t y, std::uint32_t z)
        {
            return (x & y) | (~x & z);
        }

        std::uint32_t md4_g(std::uint32_t x, std::uint32_t y, std::uint32_t z)
        {
            return (x & y) | (x & z) | (y & z);
        }

        std::uint32_t md4_h(std::uint32_t x, std::uint32_t y, std::uint32_t z)
        {
            return x ^ y ^ z;
        }

        std::vector<std::uint8_t> md4(std::vector<std::uint8_t> input)
        {
            const std::uint64_t bit_len = static_cast<std::uint64_t>(input.size()) * 8ULL;
            input.push_back(0x80);
            while ((input.size() % 64) != 56) {
                input.push_back(0);
            }
            for (int i = 0; i < 8; ++i) {
                input.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xFF));
            }

            std::uint32_t a = 0x67452301;
            std::uint32_t b = 0xefcdab89;
            std::uint32_t c = 0x98badcfe;
            std::uint32_t d = 0x10325476;

            auto round1 = [](std::uint32_t &aa, std::uint32_t bb, std::uint32_t cc,
                             std::uint32_t dd, std::uint32_t x, std::uint32_t s) {
                aa = rotl32(aa + md4_f(bb, cc, dd) + x, s);
            };
            auto round2 = [](std::uint32_t &aa, std::uint32_t bb, std::uint32_t cc,
                             std::uint32_t dd, std::uint32_t x, std::uint32_t s) {
                aa = rotl32(aa + md4_g(bb, cc, dd) + x + 0x5a827999, s);
            };
            auto round3 = [](std::uint32_t &aa, std::uint32_t bb, std::uint32_t cc,
                             std::uint32_t dd, std::uint32_t x, std::uint32_t s) {
                aa = rotl32(aa + md4_h(bb, cc, dd) + x + 0x6ed9eba1, s);
            };

            for (std::size_t offset = 0; offset < input.size(); offset += 64) {
                std::uint32_t x[16] = {};
                for (int i = 0; i < 16; ++i) {
                    const std::size_t j = offset + i * 4;
                    x[i] = static_cast<std::uint32_t>(input[j]) |
                           (static_cast<std::uint32_t>(input[j + 1]) << 8) |
                           (static_cast<std::uint32_t>(input[j + 2]) << 16) |
                           (static_cast<std::uint32_t>(input[j + 3]) << 24);
                }

                const std::uint32_t aa = a;
                const std::uint32_t bb = b;
                const std::uint32_t cc = c;
                const std::uint32_t dd = d;

                round1(a, b, c, d, x[0], 3); round1(d, a, b, c, x[1], 7);
                round1(c, d, a, b, x[2], 11); round1(b, c, d, a, x[3], 19);
                round1(a, b, c, d, x[4], 3); round1(d, a, b, c, x[5], 7);
                round1(c, d, a, b, x[6], 11); round1(b, c, d, a, x[7], 19);
                round1(a, b, c, d, x[8], 3); round1(d, a, b, c, x[9], 7);
                round1(c, d, a, b, x[10], 11); round1(b, c, d, a, x[11], 19);
                round1(a, b, c, d, x[12], 3); round1(d, a, b, c, x[13], 7);
                round1(c, d, a, b, x[14], 11); round1(b, c, d, a, x[15], 19);

                round2(a, b, c, d, x[0], 3); round2(d, a, b, c, x[4], 5);
                round2(c, d, a, b, x[8], 9); round2(b, c, d, a, x[12], 13);
                round2(a, b, c, d, x[1], 3); round2(d, a, b, c, x[5], 5);
                round2(c, d, a, b, x[9], 9); round2(b, c, d, a, x[13], 13);
                round2(a, b, c, d, x[2], 3); round2(d, a, b, c, x[6], 5);
                round2(c, d, a, b, x[10], 9); round2(b, c, d, a, x[14], 13);
                round2(a, b, c, d, x[3], 3); round2(d, a, b, c, x[7], 5);
                round2(c, d, a, b, x[11], 9); round2(b, c, d, a, x[15], 13);

                round3(a, b, c, d, x[0], 3); round3(d, a, b, c, x[8], 9);
                round3(c, d, a, b, x[4], 11); round3(b, c, d, a, x[12], 15);
                round3(a, b, c, d, x[2], 3); round3(d, a, b, c, x[10], 9);
                round3(c, d, a, b, x[6], 11); round3(b, c, d, a, x[14], 15);
                round3(a, b, c, d, x[1], 3); round3(d, a, b, c, x[9], 9);
                round3(c, d, a, b, x[5], 11); round3(b, c, d, a, x[13], 15);
                round3(a, b, c, d, x[3], 3); round3(d, a, b, c, x[11], 9);
                round3(c, d, a, b, x[7], 11); round3(b, c, d, a, x[15], 15);

                a += aa;
                b += bb;
                c += cc;
                d += dd;
            }

            std::vector<std::uint8_t> digest(16);
            const std::uint32_t words[4] = { a, b, c, d };
            for (int i = 0; i < 4; ++i) {
                digest[i * 4] = static_cast<std::uint8_t>(words[i] & 0xFF);
                digest[i * 4 + 1] = static_cast<std::uint8_t>((words[i] >> 8) & 0xFF);
                digest[i * 4 + 2] = static_cast<std::uint8_t>((words[i] >> 16) & 0xFF);
                digest[i * 4 + 3] = static_cast<std::uint8_t>((words[i] >> 24) & 0xFF);
            }
            return digest;
        }

        std::vector<std::uint8_t> ascii_to_utf16le(std::string_view text)
        {
            std::vector<std::uint8_t> out;
            out.reserve(text.size() * 2);
            for (unsigned char ch : text) {
                out.push_back(ch);
                out.push_back(0);
            }
            return out;
        }
    }

    NasAuthService::NasAuthService(std::shared_ptr<NasMetadataStore> metadata)
        : metadata_(std::move(metadata))
    {
    }

    NasAuthResult NasAuthService::authenticate_password(std::string_view username, std::string_view password) const
    {
        NasAuthResult result;
        if (!metadata_ || !metadata_->available()) {
            result.error = "metadata unavailable";
            return result;
        }
        auto user = metadata_->find_user_by_name(username);
        if (!user) {
            result.error = "user not found";
            return result;
        }
        if (!user->enabled) {
            result.error = "user disabled";
            return result;
        }
        if (!verify_password(user->password_hash, password)) {
            result.error = "invalid credentials";
            return result;
        }
        result.authenticated = true;
        result.user = std::move(*user);
        return result;
    }

    NasAuthResult NasAuthService::authenticate_basic_header(std::string_view authorization_header) const
    {
        auto credentials = parse_basic_header(authorization_header);
        if (!credentials) {
            NasAuthResult result;
            result.error = "invalid basic auth header";
            return result;
        }
        return authenticate_password(credentials->first, credentials->second);
    }

    std::string NasAuthService::hash_password_for_config(std::string_view password, std::string_view salt)
    {
        const std::string material = std::string(salt) + ":" + std::string(password);
        std::ostringstream oss;
        oss << "fnv1a64$" << salt << "$" << std::hex << fnv1a64(material);
        return oss.str();
    }

    std::string NasAuthService::nt_hash_for_config(std::string_view password)
    {
        auto digest = md4(ascii_to_utf16le(password));
        std::ostringstream oss;
        oss << "nthash:" << std::hex << std::setfill('0');
        for (std::uint8_t byte : digest) {
            oss << std::setw(2) << static_cast<int>(byte);
        }
        return oss.str();
    }

    bool NasAuthService::verify_password(std::string_view stored_hash, std::string_view password)
    {
        constexpr std::string_view plain_prefix = "plain:";
        if (stored_hash.rfind(plain_prefix, 0) == 0) {
            return constant_time_equal(stored_hash.substr(plain_prefix.size()), password);
        }

        constexpr std::string_view hash_prefix = "fnv1a64$";
        if (stored_hash.rfind(hash_prefix, 0) == 0) {
            const auto second_sep = stored_hash.find('$', hash_prefix.size());
            if (second_sep == std::string_view::npos) {
                return false;
            }
            const auto salt = stored_hash.substr(hash_prefix.size(), second_sep - hash_prefix.size());
            return constant_time_equal(stored_hash, hash_password_for_config(password, salt));
        }

        return false;
    }

    std::optional<std::pair<std::string, std::string>> NasAuthService::parse_basic_header(std::string_view authorization_header)
    {
        if (!starts_with_ci(authorization_header, "Basic ")) {
            return std::nullopt;
        }
        const std::string encoded(authorization_header.substr(6));
        const std::string decoded = yuan::base::util::base64_decode(encoded);
        const auto sep = decoded.find(':');
        if (sep == std::string::npos) {
            return std::nullopt;
        }
        return std::make_pair(decoded.substr(0, sep), decoded.substr(sep + 1));
    }
}
