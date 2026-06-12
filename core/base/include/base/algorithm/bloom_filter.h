#ifndef YUAN_BASE_ALGORITHM_BLOOM_FILTER_H_
#define YUAN_BASE_ALGORITHM_BLOOM_FILTER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <utility>
#include <vector>

namespace yuan::base
{
    // BloomFilter 是空间效率较高的概率型集合，适合“快速判断一定不存在/可能存在”。
    // 它可能误判存在，但不会误判不存在。
    //
    // 适用场景：大规模去重预筛、缓存穿透保护、URL/消息/nonce 快速过滤。
    // 用法：
    //   yuan::base::BloomFilter<std::string> filter(8192, 4);
    //   filter.add("key");
    //   if (filter.possibly_contains("key")) { ... }
    template <typename T, typename Hash = std::hash<T>>
    class BloomFilter
    {
    public:
        BloomFilter(std::size_t bit_count, std::size_t hash_count = 3, Hash hash = Hash{})
            : bits_((bit_count + 63) / 64), bit_count_(bit_count), hash_count_(hash_count), hash_(std::move(hash))
        {
        }

        void add(const T &value)
        {
            if (bit_count_ == 0 || hash_count_ == 0) {
                return;
            }

            const auto h1 = static_cast<std::uint64_t>(hash_(value));
            const auto h2 = mix(h1);
            for (std::size_t i = 0; i < hash_count_; ++i) {
                set_bit((h1 + i * h2) % bit_count_);
            }
        }

        bool possibly_contains(const T &value) const
        {
            if (bit_count_ == 0 || hash_count_ == 0) {
                return false;
            }

            const auto h1 = static_cast<std::uint64_t>(hash_(value));
            const auto h2 = mix(h1);
            for (std::size_t i = 0; i < hash_count_; ++i) {
                if (!get_bit((h1 + i * h2) % bit_count_)) {
                    return false;
                }
            }
            return true;
        }

        void clear()
        {
            std::fill(bits_.begin(), bits_.end(), 0);
        }

        std::size_t bit_count() const noexcept { return bit_count_; }
        std::size_t hash_count() const noexcept { return hash_count_; }

    private:
        static std::uint64_t mix(std::uint64_t value)
        {
            value ^= value >> 33;
            value *= 0xff51afd7ed558ccdULL;
            value ^= value >> 33;
            value *= 0xc4ceb9fe1a85ec53ULL;
            value ^= value >> 33;
            return value | 1ULL;
        }

        void set_bit(std::size_t index)
        {
            bits_[index / 64] |= (1ULL << (index % 64));
        }

        bool get_bit(std::size_t index) const
        {
            return (bits_[index / 64] & (1ULL << (index % 64))) != 0;
        }

        std::vector<std::uint64_t> bits_;
        std::size_t bit_count_ = 0;
        std::size_t hash_count_ = 0;
        Hash hash_;
    };
}

#endif // YUAN_BASE_ALGORITHM_BLOOM_FILTER_H_
