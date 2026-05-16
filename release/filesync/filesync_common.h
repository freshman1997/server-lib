#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace filesync {

inline std::uint64_t file_time_to_seconds(const std::filesystem::file_time_type& time) {
    const auto system_time = std::chrono::time_point_cast<std::chrono::seconds>(
        time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<std::uint64_t>(system_time.time_since_epoch().count());
}

inline std::string normalize_relative_path(const std::filesystem::path& path) {
    auto value = path.generic_string();
    while (!value.empty() && value.front() == '/') {
        value.erase(value.begin());
    }
    return value;
}

inline bool is_safe_relative_path(const std::string& value) {
    if (value.empty() || value[0] == '/' || value.find('\\') != std::string::npos ||
        value.find(':') != std::string::npos) {
        return false;
    }
    std::filesystem::path path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const auto& part : path) {
        const auto text = part.generic_string();
        if (text.empty() || text == "." || text == "..") {
            return false;
        }
    }
    return true;
}

inline std::uint64_t fnv1a_file_hash(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open file for hashing: " + path.string());
    }

    std::uint64_t hash = 1469598103934665603ull;
    char buffer[64 * 1024];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const auto count = in.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

inline std::string quote_token(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
            ch == '_' || ch == '.' || ch == '/') {
            out << static_cast<char>(ch);
        } else {
            static constexpr char digits[] = "0123456789ABCDEF";
            out << '%' << digits[ch >> 4] << digits[ch & 0x0f];
        }
    }
    return out.str();
}

inline int from_hex(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

inline std::string unquote_token(const std::string& value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = from_hex(value[i + 1]);
            const int lo = from_hex(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

inline std::vector<std::string> split_line(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> parts;
    std::string part;
    while (in >> part) {
        parts.push_back(part);
    }
    return parts;
}

inline std::string hex_encode(const std::vector<char>& data) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char ch : data) {
        out.push_back(digits[ch >> 4]);
        out.push_back(digits[ch & 0x0f]);
    }
    return out;
}

inline std::vector<char> hex_decode(const std::string& text) {
    if (text.size() % 2 != 0) {
        throw std::runtime_error("invalid hex payload");
    }
    std::vector<char> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = from_hex(text[i]);
        const int lo = from_hex(text[i + 1]);
        if (hi < 0 || lo < 0) {
            throw std::runtime_error("invalid hex payload");
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

inline std::vector<char> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open file: " + path.string());
    }
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace filesync
