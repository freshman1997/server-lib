#include "yuan/rpc/security.h"

#include <utility>

namespace yuan::rpc::security
{
    XorStreamCipher::XorStreamCipher(std::string key)
        : key_(std::move(key))
    {
    }

    bool XorStreamCipher::operator()(const wire::CryptoContext &context, const Bytes &input, Bytes &output) const
    {
        if (key_.empty() || context.encryption != Encryption::xor_stream) {
            return false;
        }

        output.resize(input.size());
        std::uint64_t state = context.nonce ^ (static_cast<std::uint64_t>(context.key_id) << 32U) ^ context.request_id;
        for (std::size_t i = 0; i < input.size(); ++i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            const auto key_byte = static_cast<std::uint8_t>(key_[i % key_.size()]);
            const auto stream_byte = static_cast<std::uint8_t>((state >> 56U) & 0xFFU);
            output[i] = input[i] ^ key_byte ^ stream_byte;
        }
        return true;
    }
}
