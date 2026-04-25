#include "nas/nas_auth_service.h"

#include "base/utils/base64.h"

#include <cstdint>
#include <sstream>

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
