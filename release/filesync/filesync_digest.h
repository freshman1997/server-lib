#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace filesync::digest {
std::string sha256_bytes(const char* data, std::size_t size);
std::string sha256_file(const std::filesystem::path& path);
}

namespace filesync {
std::string sha256_bytes(const char* data, std::size_t size);
std::string sha256_file(const std::filesystem::path& path);
}
