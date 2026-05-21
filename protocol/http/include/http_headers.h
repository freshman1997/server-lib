#ifndef __NET_HTTP_HTTP_HEADERS_H__
#define __NET_HTTP_HTTP_HEADERS_H__

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace yuan::net::http
{
    inline char ascii_lower(char ch) noexcept
    {
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A')) : ch;
    }

    inline bool header_key_equals_ci(std::string_view lhs, std::string_view rhs) noexcept
    {
        if (lhs.size() != rhs.size()) {
            return false;
        }

        for (std::size_t i = 0; i < lhs.size(); ++i) {
            if (ascii_lower(lhs[i]) != ascii_lower(rhs[i])) {
                return false;
            }
        }
        return true;
    }

    inline std::size_t header_key_hash_ci(std::string_view key) noexcept
    {
        std::size_t hash = 1469598103934665603ull;
        for (const char ch : key) {
            hash ^= static_cast<unsigned char>(ascii_lower(ch));
            hash *= 1099511628211ull;
        }
        return hash;
    }

    struct HttpHeaderKeyHash
    {
        using is_transparent = void;

        std::size_t operator()(const std::string &key) const noexcept
        {
            return header_key_hash_ci(key);
        }

        std::size_t operator()(std::string_view key) const noexcept
        {
            return header_key_hash_ci(key);
        }

        std::size_t operator()(const char *key) const noexcept
        {
            return key ? header_key_hash_ci(std::string_view(key)) : 0;
        }
    };

    struct HttpHeaderKeyEq
    {
        using is_transparent = void;

        bool operator()(const std::string &lhs, const std::string &rhs) const noexcept
        {
            return header_key_equals_ci(lhs, rhs);
        }

        bool operator()(std::string_view lhs, std::string_view rhs) const noexcept
        {
            return header_key_equals_ci(lhs, rhs);
        }

        bool operator()(const std::string &lhs, std::string_view rhs) const noexcept
        {
            return header_key_equals_ci(lhs, rhs);
        }

        bool operator()(std::string_view lhs, const std::string &rhs) const noexcept
        {
            return header_key_equals_ci(lhs, rhs);
        }

        bool operator()(const char *lhs, const std::string &rhs) const noexcept
        {
            return lhs ? header_key_equals_ci(lhs, rhs) : rhs.empty();
        }

        bool operator()(const std::string &lhs, const char *rhs) const noexcept
        {
            return rhs ? header_key_equals_ci(lhs, rhs) : lhs.empty();
        }
    };

    using HttpHeaderMap = std::unordered_map<std::string, std::string, HttpHeaderKeyHash, HttpHeaderKeyEq>;
}

#endif
