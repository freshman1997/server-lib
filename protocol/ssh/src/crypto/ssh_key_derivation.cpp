#include "crypto/ssh_key_derivation.h"
#include "crypto/ssh_crypto_openssl.h"
#include <cstring>

namespace yuan::net::ssh
{
    static void append_uint32_be(std::vector<uint8_t> & buf, uint32_t val)
    {
        buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
    }

    static void append_string(std::vector<uint8_t> & buf, const std::string & s)
    {
        append_uint32_be(buf, static_cast<uint32_t>(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }

    static void append_mpint(std::vector<uint8_t> & buf, const std::vector<uint8_t> & val)
    {
        size_t offset = 0;
        while (offset < val.size() && val[offset] == 0)
            ++offset;

        auto trimmed = val.size() - offset;
        bool needs_pad = (trimmed > 0 && (val[offset] & 0x80) != 0);

        uint32_t len = static_cast<uint32_t>(trimmed + (needs_pad ? 1 : 0));
        append_uint32_be(buf, len);

        if (needs_pad)
            buf.push_back(0x00);

        buf.insert(buf.end(), val.begin() + offset, val.end());
    }

    static void append_raw(std::vector<uint8_t> & buf, const std::vector<uint8_t> & data)
    {
        append_uint32_be(buf, static_cast<uint32_t>(data.size()));
        buf.insert(buf.end(), data.begin(), data.end());
    }

    static std::vector<uint8_t> compute_hash(SshCryptoOpenSSL & crypto,
                                             const std::string & hash_name,
                                             const uint8_t * data, size_t len)
    {
        if (hash_name == "sha512")
            return crypto.sha512(data, len);
        if (hash_name == "sha384")
            return crypto.sha384(data, len);
        if (hash_name == "sha1")
            return crypto.sha1(data, len);
        return crypto.sha256(data, len);
    }

    std::vector<uint8_t> SshKeyDerivation::derive_key(
        const std::vector<uint8_t> & K,
        const std::vector<uint8_t> & H,
        const std::vector<uint8_t> & session_id,
        char letter,
        size_t key_len,
        const std::string & hash_name)
    {
        SshCryptoOpenSSL crypto;

        std::vector<uint8_t> input;
        append_mpint(input, K);
        input.insert(input.end(), H.begin(), H.end());
        input.push_back(static_cast<uint8_t>(letter));
        input.insert(input.end(), session_id.begin(), session_id.end());

        std::vector<uint8_t> hash = compute_hash(crypto, hash_name, input.data(), input.size());

        if (hash.size() >= key_len) {
            hash.resize(key_len);
            return hash;
        }

        std::vector<uint8_t> result;
        result.insert(result.end(), hash.begin(), hash.end());

        while (result.size() < key_len) {
            std::vector<uint8_t> extend_input;
            append_mpint(extend_input, K);
            extend_input.insert(extend_input.end(), H.begin(), H.end());
            extend_input.insert(extend_input.end(), result.begin(), result.end());

            hash = compute_hash(crypto, hash_name, extend_input.data(), extend_input.size());

            size_t to_copy = std::min(hash.size(), key_len - result.size());
            result.insert(result.end(), hash.begin(), hash.begin() + to_copy);
        }

        result.resize(key_len);
        return result;
    }

    std::vector<uint8_t> SshKeyDerivation::compute_exchange_hash_sha256(
        const std::string & client_version,
        const std::string & server_version,
        const std::vector<uint8_t> & client_kex_init,
        const std::vector<uint8_t> & server_kex_init,
        const std::vector<uint8_t> & host_key,
        const std::vector<uint8_t> & client_public,
        const std::vector<uint8_t> & server_public,
        const std::vector<uint8_t> & shared_secret)
    {
        std::vector<uint8_t> hash_input;
        append_string(hash_input, client_version);
        append_string(hash_input, server_version);
        append_raw(hash_input, client_kex_init);
        append_raw(hash_input, server_kex_init);
        append_raw(hash_input, host_key);
        append_mpint(hash_input, client_public);
        append_mpint(hash_input, server_public);
        append_mpint(hash_input, shared_secret);

        SshCryptoOpenSSL crypto;
        return crypto.sha256(hash_input.data(), hash_input.size());
    }

    std::vector<uint8_t> SshKeyDerivation::compute_exchange_hash_sha384(
        const std::string & client_version,
        const std::string & server_version,
        const std::vector<uint8_t> & client_kex_init,
        const std::vector<uint8_t> & server_kex_init,
        const std::vector<uint8_t> & host_key,
        const std::vector<uint8_t> & client_public,
        const std::vector<uint8_t> & server_public,
        const std::vector<uint8_t> & shared_secret)
    {
        std::vector<uint8_t> hash_input;
        append_string(hash_input, client_version);
        append_string(hash_input, server_version);
        append_raw(hash_input, client_kex_init);
        append_raw(hash_input, server_kex_init);
        append_raw(hash_input, host_key);
        append_mpint(hash_input, client_public);
        append_mpint(hash_input, server_public);
        append_mpint(hash_input, shared_secret);

        SshCryptoOpenSSL crypto;
        return crypto.sha384(hash_input.data(), hash_input.size());
    }

    std::vector<uint8_t> SshKeyDerivation::compute_exchange_hash_sha512(
        const std::string & client_version,
        const std::string & server_version,
        const std::vector<uint8_t> & client_kex_init,
        const std::vector<uint8_t> & server_kex_init,
        const std::vector<uint8_t> & host_key,
        const std::vector<uint8_t> & client_public,
        const std::vector<uint8_t> & server_public,
        const std::vector<uint8_t> & shared_secret)
    {
        std::vector<uint8_t> hash_input;
        append_string(hash_input, client_version);
        append_string(hash_input, server_version);
        append_raw(hash_input, client_kex_init);
        append_raw(hash_input, server_kex_init);
        append_raw(hash_input, host_key);
        append_mpint(hash_input, client_public);
        append_mpint(hash_input, server_public);
        append_mpint(hash_input, shared_secret);

        SshCryptoOpenSSL crypto;
        return crypto.sha512(hash_input.data(), hash_input.size());
    }

    std::vector<uint8_t> SshKeyDerivation::derive_session_id(const std::vector<uint8_t> & exchange_hash)
    {
        return exchange_hash;
    }
}
