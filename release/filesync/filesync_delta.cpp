#include "filesync_delta.h"

#include "filesync_common.h"
#include "filesync_limits.h"

#include <array>
#include <fstream>
#include <unordered_map>

namespace filesync::delta
{
    constexpr std::uint32_t kMod = 65521;

    std::uint32_t weak_checksum(const char *data, std::size_t size)
    {
        std::uint32_t a = 1;
        std::uint32_t b = 0;
        for (std::size_t i = 0; i < size; ++i) {
            a = (a + static_cast<unsigned char>(data[i])) % kMod;
            b = (b + a) % kMod;
        }
        return (b << 16) | a;
    }
}

namespace filesync::delta
{
    FileSignature create_signature(const std::filesystem::path &path, std::uint32_t block_size)
    {
        if (block_size == 0) {
            throw std::runtime_error("delta block size must be positive");
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("failed to open file for delta signature: " + path.string());
        }

        FileSignature result;
        result.block_size = block_size;
        std::vector<char> block(block_size);
        std::uint32_t index = 0;
        while (input.read(block.data(), static_cast<std::streamsize>(block.size())) || input.gcount() != 0) {
            const auto size = static_cast<std::size_t>(input.gcount());
            result.blocks.push_back({index++, weak_checksum(block.data(), size),
                                     filesync::digest::sha256_bytes(block.data(), size),
                                     static_cast<std::uint32_t>(size)});
            result.file_size += size;

            if (size != block.size()) break;
        }

        result.file_hash = filesync::digest::sha256_file(path);

        return result;
    }

    class OperationStream::Impl
    {
    public:
        Impl(const std::filesystem::path &source, const FileSignature &basis_value)
            : basis(basis_value), input(source, std::ios::binary), window(basis.block_size)
        {
            if (basis.block_size == 0) {
                throw std::runtime_error("cannot create delta without a block size");
            }

            if (!input) {
                throw std::runtime_error("failed to open file for delta: " + source.string());
            }

            for (const auto &block : basis.blocks) {
                candidates[(static_cast<std::uint64_t>(block.weak) << 32) | block.size].push_back(&block);
            }

            input.read(window.data(), static_cast<std::streamsize>(window.size()));
            available = static_cast<std::size_t>(input.gcount());
        }

        bool next(Operation &operation)
        {
            operation = Operation{};
            while (available != 0) {
                const auto key = (static_cast<std::uint64_t>(weak_checksum(window.data(), available)) << 32) | available;
                const BlockSignature *match = nullptr;
                const auto it = candidates.find(key);
                if (it != candidates.end()) {
                    const auto strong = filesync::digest::sha256_bytes(window.data(), available);
                    for (const auto *candidate : it->second) {
                        if (candidate->strong == strong) {
                            match = candidate;
                            break;
                        }
                    }
                }

                if (match != nullptr) {
                    if (!pending.empty()) {
                        return take_data(operation);
                    }

                    operation.kind = Operation::Kind::Copy;
                    operation.offset = static_cast<std::uintmax_t>(match->index) * basis.block_size;
                    operation.size = match->size;
                    input.read(window.data(), static_cast<std::streamsize>(window.size()));
                    available = static_cast<std::size_t>(input.gcount());

                    return true;
                }

                pending.push_back(window[0]);
                if (available > 1) {
                    std::move(window.begin() + 1, window.begin() + static_cast<std::ptrdiff_t>(available), window.begin());
                }

                input.read(window.data() + (available > 1 ? available - 1 : 0), 1);
                if (input.gcount() == 1) {
                    available = std::min<std::size_t>(basis.block_size, available + 1);
                } else {
                    --available;
                }

                if (pending.size() == limits::max_delta_data_size) {
                    return take_data(operation);
                }
            }

            return !pending.empty() ? take_data(operation) : false;
        }

    private:
        bool take_data(Operation &operation)
        {
            operation.kind = Operation::Kind::Data;
            operation.size = static_cast<std::uint32_t>(pending.size());
            operation.data.swap(pending);
            return true;
        }

        const FileSignature &basis;
        std::ifstream input;
        std::unordered_map<std::uint64_t, std::vector<const BlockSignature *>> candidates;
        std::vector<char> window;
        std::vector<char> pending;
        std::size_t available = 0;
    };

    OperationStream::OperationStream(const std::filesystem::path &source, const FileSignature &basis)
        : impl_(std::make_unique<Impl>(source, basis)) {}
    OperationStream::~OperationStream() = default;
    OperationStream::OperationStream(OperationStream &&) noexcept = default;
    OperationStream &OperationStream::operator=(OperationStream &&) noexcept = default;
    bool OperationStream::next(Operation &operation) { return impl_->next(operation); }

    void stream_delta(const std::filesystem::path &source, const FileSignature &basis, OperationConsumer &consumer)
    {
        OperationStream stream(source, basis);
        Operation operation;
        while (stream.next(operation)) {
            if (operation.kind == Operation::Kind::Copy) {
                consumer.copy(operation.offset, operation.size);
            } else {
                consumer.data(operation.data.data(), operation.data.size());
            }
        }
    }

} // namespace filesync::delta
