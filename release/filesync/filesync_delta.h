#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace filesync::delta {

// A signature is deliberately small: the receiver never sends file contents
// while advertising a basis, only one record per basis block.
struct BlockSignature {
    std::uint32_t index = 0;
    std::uint32_t weak = 0;
    std::string strong; // SHA-256, lower-case hexadecimal
    std::uint32_t size = 0;
};

struct FileSignature {
    std::uint32_t block_size = 0;
    std::uintmax_t file_size = 0;
    std::string file_hash; // SHA-256, lower-case hexadecimal
    std::vector<BlockSignature> blocks;
};

struct Operation {
    enum class Kind { Copy, Data };
    Kind kind = Kind::Data;
    std::uintmax_t offset = 0;
    std::uint32_t size = 0;
    std::vector<char> data;
};

class OperationConsumer {
public:
    virtual ~OperationConsumer() = default;
    virtual void copy(std::uintmax_t offset, std::uint32_t size) = 0;
    virtual void data(const char* bytes, std::size_t size) = 0;
};

class OperationStream {
public:
    OperationStream(const std::filesystem::path& source, const FileSignature& basis);
    ~OperationStream();
    OperationStream(OperationStream&&) noexcept;
    OperationStream& operator=(OperationStream&&) noexcept;
    OperationStream(const OperationStream&) = delete;
    OperationStream& operator=(const OperationStream&) = delete;

    bool next(Operation& operation);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

FileSignature create_signature(const std::filesystem::path& path, std::uint32_t block_size);
void stream_delta(const std::filesystem::path& source, const FileSignature& basis, OperationConsumer& consumer);

} // namespace filesync::delta
