#ifndef __BASE_UTILS_STRING_UTILS_H__
#define __BASE_UTILS_STRING_UTILS_H__
#include <cstdint>
#include <cctype>
#include <ios>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace yuan::base::util
{
    inline bool is_ascii_space(char ch)
    {
        switch (ch) {
        case ' ':
        case '\t':
        case '\r':
        case '\n':
        case '\f':
        case '\v':
            return true;
        default:
            return false;
        }
    }

    inline char lower_ascii_char(char ch)
    {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    }

    inline char upper_ascii_char(char ch)
    {
        return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
    }

    inline std::string_view trim_ascii(std::string_view value)
    {
        while (!value.empty() && is_ascii_space(value.front())) {
            value.remove_prefix(1);
        }
        while (!value.empty() && is_ascii_space(value.back())) {
            value.remove_suffix(1);
        }
        return value;
    }

    inline std::string trim_ascii_copy(std::string_view value)
    {
        const auto trimmed = trim_ascii(value);
        return std::string(trimmed);
    }

    inline void lower_ascii_inplace(std::string &value)
    {
        for (char &ch : value) {
            ch = lower_ascii_char(ch);
        }
    }

    inline void upper_ascii_inplace(std::string &value)
    {
        for (char &ch : value) {
            ch = upper_ascii_char(ch);
        }
    }

    inline std::string lower_ascii(std::string_view value)
    {
        std::string out(value);
        lower_ascii_inplace(out);
        return out;
    }

    inline std::string upper_ascii(std::string_view value)
    {
        std::string out(value);
        upper_ascii_inplace(out);
        return out;
    }

    inline bool iequals_ascii(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            if (lower_ascii_char(lhs[i]) != lower_ascii_char(rhs[i])) {
                return false;
            }
        }
        return true;
    }

    inline bool starts_with(std::string_view value, std::string_view prefix)
    {
        return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    inline bool ends_with(std::string_view value, std::string_view suffix)
    {
        return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
    }

    inline bool starts_with_ascii_ci(std::string_view value, std::string_view prefix)
    {
        return value.size() >= prefix.size() && iequals_ascii(value.substr(0, prefix.size()), prefix);
    }

    inline bool ends_with_ascii_ci(std::string_view value, std::string_view suffix)
    {
        return value.size() >= suffix.size() && iequals_ascii(value.substr(value.size() - suffix.size()), suffix);
    }

    inline bool contains_ascii_ci(std::string_view haystack, std::string_view needle)
    {
        if (needle.empty()) {
            return true;
        }
        if (needle.size() > haystack.size()) {
            return false;
        }

        for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
            if (iequals_ascii(haystack.substr(i, needle.size()), needle)) {
                return true;
            }
        }
        return false;
    }

    inline std::vector<std::string_view> split_view(std::string_view value, char delimiter, bool keep_empty = true)
    {
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        while (start <= value.size()) {
            const std::size_t pos = value.find(delimiter, start);
            const std::size_t end = pos == std::string_view::npos ? value.size() : pos;
            auto part = value.substr(start, end - start);
            if (keep_empty || !part.empty()) {
                parts.push_back(part);
            }
            if (pos == std::string_view::npos) {
                break;
            }
            start = pos + 1;
        }
        return parts;
    }

    inline std::vector<std::string> split(std::string_view value, char delimiter, bool keep_empty = true)
    {
        std::vector<std::string> parts;
        for (auto part : split_view(value, delimiter, keep_empty)) {
            parts.emplace_back(part);
        }
        return parts;
    }

    inline std::string join(std::span<const std::string> parts, std::string_view delimiter)
    {
        std::size_t size = 0;
        for (const auto &part : parts) {
            size += part.size();
        }
        if (!parts.empty()) {
            size += delimiter.size() * (parts.size() - 1);
        }

        std::string out;
        out.reserve(size);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i != 0) {
                out.append(delimiter);
            }
            out.append(parts[i]);
        }
        return out;
    }

    inline std::string join(const std::vector<std::string> &parts, std::string_view delimiter)
    {
        return join(std::span<const std::string>(parts.data(), parts.size()), delimiter);
    }

    inline bool start_with_ignore_case(const char *begin, const char *end, const char *str)
    {
        const char *p = begin;
        const char *p1 = str;
        for (;p != end && *p1; ++p, ++p1) {
            if (std::tolower(*p) != *p1) {
                return false;
            }
        }
        return p == end && !(*p1);
    }

    inline bool start_with(const char *begin, const char *end, const char *str)
    {
        const char *p = begin;
        const char *p1 = str;
        for (;p != end && *p1; ++p, ++p1) {
            if (*p != *p1) {
                return false;
            }
        }
        return p == end && !(*p1);
    }

    inline int end_with(const char *begin, const char *end, const char *str)
    {
        const char *p = begin;
        const char *p1 = str;
        for (;p != end && *p1; ++p) {
            if (*p == *p1) {
                ++p1; ++p;
                for (; *p1 && *p == *p1 && p != end; ) {
                    ++p1; ++p;
                }
                if (p == end && !(*p1)) {
                    return true;
                }
            }
        }
        return -1;
    }

    inline int find_first(const char *begin, const char *end, const char *str)
    {
        const char *p = begin;
        const char *p1 = str;
        for (;p != end && *p1; ++p) {
            for (; *p1 && *p == *p1 && p < end; ++p1, ++p);
        }
        return !(*p1) ? (p - begin)  - (p1 - str) - 1: -1;
    }

    template<typename T>
    std::string to_hex_string(T t)
    {
        std::stringstream ss;
        ss << std::hex << t;
        return ss.str();
    }

    inline std::string to_hex(std::span<const std::uint8_t> bytes)
    {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (std::uint8_t byte : bytes) {
            hex.push_back(kHex[(byte >> 4) & 0x0F]);
            hex.push_back(kHex[byte & 0x0F]);
        }
        return hex;
    }

    inline std::string to_hex(const std::vector<std::uint8_t> &bytes)
    {
        return to_hex(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    }

    inline std::string to_hex(const std::uint8_t *data, std::size_t len)
    {
        return to_hex(std::span<const std::uint8_t>(data, len));
    }

    inline int from_hex_digit(char ch)
    {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return 10 + (ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F') {
            return 10 + (ch - 'A');
        }
        return -1;
    }

    inline std::vector<std::uint8_t> from_hex(const std::string &hex)
    {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(hex.size() / 2);
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
            const int hi = from_hex_digit(hex[i]);
            const int lo = from_hex_digit(hex[i + 1]);
            if (hi < 0 || lo < 0) {
                continue;
            }
            bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        }
        return bytes;
    }
}

#endif
