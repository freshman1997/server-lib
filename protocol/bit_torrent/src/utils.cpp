#include "utils.h"
#include "openssl/sha.h"
#include <cstring>
#include <random>
#include <chrono>

namespace yuan::net::bit_torrent
{

std::vector<uint8_t> sha1_hash(const unsigned char *input, std::size_t len)
{
    std::vector<uint8_t> hash(20);
    SHA1(input, len, hash.data());
    return hash;
}

std::vector<uint8_t> sha1_hash(const std::string &data)
{
    return sha1_hash(reinterpret_cast<const unsigned char *>(data.data()), data.size());
}

std::string to_hex(const std::vector<uint8_t> &bytes)
{
    return to_hex(bytes.data(), bytes.size());
}

std::string to_hex(const uint8_t *data, size_t len)
{
    std::string hex;
    hex.reserve(len * 2);
    for (size_t i = 0; i < len; i++)
    {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", data[i]);
        hex += buf;
    }
    return hex;
}

std::vector<uint8_t> from_hex(const std::string &hex)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        unsigned int byte;
        sscanf(hex.c_str() + i, "%02x", &byte);
        bytes.push_back(static_cast<uint8_t>(byte));
    }
    return bytes;
}

std::string url_encode(const std::string &str)
{
    std::string result;
    result.reserve(str.size() * 3);
    for (unsigned char ch : str)
    {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~')
        {
            result += static_cast<char>(ch);
        }
        else
        {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%.2X", ch);
            result += buf;
        }
    }
    return result;
}

std::string generate_peer_id()
{
    // AZ-style: -YZ0001-<12 random chars>  (20 bytes total)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 9);

    std::string peer_id = "-YZ0001-";
    for (int i = 0; i < 12; i++)
        peer_id += static_cast<char>('0' + dis(gen));
    return peer_id;
}

int64_t current_timestamp_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace yuan::net::bit_torrent
