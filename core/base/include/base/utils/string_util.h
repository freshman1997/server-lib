#ifndef __BASE_UTILS_STRING_UTILS_H__
#define __BASE_UTILS_STRING_UTILS_H__
#include <cstdint>
#include <cctype>
#include <ios>
#include <sstream>
#include <span>
#include <string>
#include <vector>

namespace yuan::base::util
{
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

    static int end_with(const char *begin, const char *end, const char *str)
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
