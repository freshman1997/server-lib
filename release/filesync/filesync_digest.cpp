#include "filesync_digest.h"
#include "filesync_limits.h"

#include <array>
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <stdexcept>

namespace filesync::digest 
{
    std::string hex_digest(const unsigned char* value, unsigned int length) 
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(length * 2);
        for (unsigned int i = 0; i < length; ++i) {
            result.push_back(digits[value[i] >> 4]);
            result.push_back(digits[value[i] & 15]);
        }
        return result;
    }

    std::string sha256_bytes(const char* data, std::size_t size) 
    {
        EVP_MD_CTX* raw = EVP_MD_CTX_new();
        if (raw == nullptr) {
            throw std::runtime_error("failed to create SHA-256 context");
        }

        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw, &EVP_MD_CTX_free);
        if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
            EVP_DigestUpdate(context.get(), data, size) != 1) {
            throw std::runtime_error("failed to calculate SHA-256");
        }

        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (EVP_DigestFinal_ex(context.get(), digest, &length) != 1) {
            throw std::runtime_error("failed to finalize SHA-256");
        }

        return hex_digest(digest, length);
    }

    std::string sha256_file(const std::filesystem::path& path) 
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("failed to open file for SHA-256: " + path.string());
        }
        
        EVP_MD_CTX* raw = EVP_MD_CTX_new();
        if (raw == nullptr) {
            throw std::runtime_error("failed to create SHA-256 context");
        }

        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw, &EVP_MD_CTX_free);
        if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error("failed to initialize SHA-256");
        }

        std::array<char, limits::digest_buffer_size> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0 && EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1) {
                throw std::runtime_error("failed to update SHA-256");
            }
        }

        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (EVP_DigestFinal_ex(context.get(), digest, &length) != 1) {
            throw std::runtime_error("failed to finalize SHA-256");
        }

        return hex_digest(digest, length);
    }
}

namespace filesync 
{
    std::string sha256_bytes(const char* data, std::size_t size) { return digest::sha256_bytes(data, size); }
    std::string sha256_file(const std::filesystem::path& path) { return digest::sha256_file(path); }
}
