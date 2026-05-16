#include "auth/smb_ntlm.h"
#include "openssl/evp.h"
#include "openssl/hmac.h"
#include "openssl/md4.h"
#include "openssl/rand.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <cctype>

namespace yuan::net::smb
{
    static uint16_t read_u16_le(const uint8_t * p)
    {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }
    static uint32_t read_u32_le(const uint8_t * p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
    static void write_u16_le(uint8_t * p, uint16_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
    }
    static void write_u32_le(uint8_t * p, uint32_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
    }

    static std::vector<uint8_t> rc4_decrypt(const std::vector<uint8_t> &key, const std::vector<uint8_t> &data)
    {
        if (key.empty() || data.empty()) return {};
        uint8_t S[256];
        for (int i = 0; i < 256; ++i) S[i] = static_cast<uint8_t>(i);
        int j = 0;
        for (int i = 0; i < 256; ++i) {
            j = (j + S[i] + key[i % key.size()]) & 0xFF;
            std::swap(S[i], S[j]);
        }
        std::vector<uint8_t> out(data.size());
        int i2 = 0, j2 = 0;
        for (size_t n = 0; n < data.size(); ++n) {
            i2 = (i2 + 1) & 0xFF;
            j2 = (j2 + S[i2]) & 0xFF;
            std::swap(S[i2], S[j2]);
            out[n] = data[n] ^ S[(S[i2] + S[j2]) & 0xFF];
        }
        return out;
    }

    static bool constant_time_equal(const std::vector<uint8_t> &lhs, const std::vector<uint8_t> &rhs)
    {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        uint8_t diff = 0;
        for (size_t i = 0; i < lhs.size(); ++i) {
            diff |= static_cast<uint8_t>(lhs[i] ^ rhs[i]);
        }
        return diff == 0;
    }

    static uint32_t rotl32(uint32_t value, uint32_t bits)
    {
        return (value << bits) | (value >> (32 - bits));
    }

    static uint32_t md4_f(uint32_t x, uint32_t y, uint32_t z)
    {
        return (x & y) | (~x & z);
    }

    static uint32_t md4_g(uint32_t x, uint32_t y, uint32_t z)
    {
        return (x & y) | (x & z) | (y & z);
    }

    static uint32_t md4_h(uint32_t x, uint32_t y, uint32_t z)
    {
        return x ^ y ^ z;
    }

    static std::vector<uint8_t> md4_internal(const std::vector<uint8_t> &input)
    {
        std::vector<uint8_t> msg = input;
        const uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8ULL;
        msg.push_back(0x80);
        while ((msg.size() % 64) != 56) {
            msg.push_back(0);
        }
        for (int i = 0; i < 8; ++i) {
            msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));
        }

        uint32_t a = 0x67452301;
        uint32_t b = 0xefcdab89;
        uint32_t c = 0x98badcfe;
        uint32_t d = 0x10325476;

        auto round1 = [](uint32_t &aa, uint32_t bb, uint32_t cc, uint32_t dd, uint32_t x, uint32_t s) {
            aa = rotl32(aa + md4_f(bb, cc, dd) + x, s);
        };
        auto round2 = [](uint32_t &aa, uint32_t bb, uint32_t cc, uint32_t dd, uint32_t x, uint32_t s) {
            aa = rotl32(aa + md4_g(bb, cc, dd) + x + 0x5a827999, s);
        };
        auto round3 = [](uint32_t &aa, uint32_t bb, uint32_t cc, uint32_t dd, uint32_t x, uint32_t s) {
            aa = rotl32(aa + md4_h(bb, cc, dd) + x + 0x6ed9eba1, s);
        };

        for (size_t offset = 0; offset < msg.size(); offset += 64) {
            uint32_t x[16] = {};
            for (int i = 0; i < 16; ++i) {
                const size_t j = offset + i * 4;
                x[i] = static_cast<uint32_t>(msg[j]) |
                       (static_cast<uint32_t>(msg[j + 1]) << 8) |
                       (static_cast<uint32_t>(msg[j + 2]) << 16) |
                       (static_cast<uint32_t>(msg[j + 3]) << 24);
            }

            uint32_t aa = a;
            uint32_t bb = b;
            uint32_t cc = c;
            uint32_t dd = d;

            round1(a, b, c, d, x[0], 3); round1(d, a, b, c, x[1], 7);
            round1(c, d, a, b, x[2], 11); round1(b, c, d, a, x[3], 19);
            round1(a, b, c, d, x[4], 3); round1(d, a, b, c, x[5], 7);
            round1(c, d, a, b, x[6], 11); round1(b, c, d, a, x[7], 19);
            round1(a, b, c, d, x[8], 3); round1(d, a, b, c, x[9], 7);
            round1(c, d, a, b, x[10], 11); round1(b, c, d, a, x[11], 19);
            round1(a, b, c, d, x[12], 3); round1(d, a, b, c, x[13], 7);
            round1(c, d, a, b, x[14], 11); round1(b, c, d, a, x[15], 19);

            round2(a, b, c, d, x[0], 3); round2(d, a, b, c, x[4], 5);
            round2(c, d, a, b, x[8], 9); round2(b, c, d, a, x[12], 13);
            round2(a, b, c, d, x[1], 3); round2(d, a, b, c, x[5], 5);
            round2(c, d, a, b, x[9], 9); round2(b, c, d, a, x[13], 13);
            round2(a, b, c, d, x[2], 3); round2(d, a, b, c, x[6], 5);
            round2(c, d, a, b, x[10], 9); round2(b, c, d, a, x[14], 13);
            round2(a, b, c, d, x[3], 3); round2(d, a, b, c, x[7], 5);
            round2(c, d, a, b, x[11], 9); round2(b, c, d, a, x[15], 13);

            round3(a, b, c, d, x[0], 3); round3(d, a, b, c, x[8], 9);
            round3(c, d, a, b, x[4], 11); round3(b, c, d, a, x[12], 15);
            round3(a, b, c, d, x[2], 3); round3(d, a, b, c, x[10], 9);
            round3(c, d, a, b, x[6], 11); round3(b, c, d, a, x[14], 15);
            round3(a, b, c, d, x[1], 3); round3(d, a, b, c, x[9], 9);
            round3(c, d, a, b, x[5], 11); round3(b, c, d, a, x[13], 15);
            round3(a, b, c, d, x[3], 3); round3(d, a, b, c, x[11], 9);
            round3(c, d, a, b, x[7], 11); round3(b, c, d, a, x[15], 15);

            a += aa;
            b += bb;
            c += cc;
            d += dd;
        }

        std::vector<uint8_t> digest(16);
        const uint32_t words[4] = { a, b, c, d };
        for (int i = 0; i < 4; ++i) {
            digest[i * 4] = static_cast<uint8_t>(words[i] & 0xFF);
            digest[i * 4 + 1] = static_cast<uint8_t>((words[i] >> 8) & 0xFF);
            digest[i * 4 + 2] = static_cast<uint8_t>((words[i] >> 16) & 0xFF);
            digest[i * 4 + 3] = static_cast<uint8_t>((words[i] >> 24) & 0xFF);
        }
        return digest;
    }

    static std::vector<uint8_t> to_utf16le(const std::string & s)
    {
        std::vector<uint8_t> result;
        result.reserve(s.size() * 2);
        for (char c : s) {
            result.push_back(static_cast<uint8_t>(c));
            result.push_back(0);
        }
        return result;
    }

    static std::string from_utf16le(const uint8_t * data, size_t len)
    {
        std::string result;
        result.reserve(len / 2);
        for (size_t i = 0; i + 1 < len; i += 2) {
            result.push_back(static_cast<char>(data[i]));
        }
        return result;
    }

    SmbNtlmAuth::SmbNtlmAuth(const std::string & server_name, const std::string & domain_name)
        : server_name_(server_name), domain_name_(domain_name)
    {
        negotiate_flags_ = NTLMSSP_NEGOTIATE_56 | NTLMSSP_NEGOTIATE_128 |
                           NTLMSSP_NEGOTIATE_KEY_EXCH |
                           NTLMSSP_NEGOTIATE_EXTENDED_SESSIONSECURITY |
                           NTLMSSP_TARGET_TYPE_SERVER |
                           NTLMSSP_NEGOTIATE_NTLM |
                           NTLMSSP_NEGOTIATE_ALWAYS_SIGN |
                           NTLMSSP_NEGOTIATE_UNICODE |
                           NTLMSSP_NEGOTIATE_SIGN |
                           NTLMSSP_REQUEST_TARGET |
                           NTLMSSP_NEGOTIATE_TARGET_INFO;
    }

    bool SmbNtlmAuth::is_complete() const
    {
        return state_ == AuthState::authenticated || state_ == AuthState::failed;
    }

    void SmbNtlmAuth::set_credentials_db(std::function<bool(const std::string &, const std::string &, const std::string &)> validator)
    {
        credential_validator_ = std::move(validator);
    }

    void SmbNtlmAuth::set_password_lookup(std::function<std::optional<std::string>(const std::string &, const std::string &)> lookup)
    {
        password_lookup_ = std::move(lookup);
    }

    void SmbNtlmAuth::set_nt_hash_lookup(std::function<std::optional<std::string>(const std::string &, const std::string &)> lookup)
    {
        nt_hash_lookup_ = std::move(lookup);
    }

    std::array<uint8_t, 8> SmbNtlmAuth::generate_server_challenge()
    {
        std::array<uint8_t, 8> challenge{};
        RAND_bytes(challenge.data(), 8);
        return challenge;
    }

    std::vector<uint8_t> SmbNtlmAuth::md4(const std::vector<uint8_t> & data)
    {
        return md4_internal(data);
    }

    std::vector<uint8_t> SmbNtlmAuth::md5(const std::vector<uint8_t> & data)
    {
        std::vector<uint8_t> digest(16);
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
        EVP_DigestUpdate(ctx, data.data(), data.size());
        EVP_DigestFinal_ex(ctx, digest.data(), nullptr);
        EVP_MD_CTX_free(ctx);
        return digest;
    }

    std::vector<uint8_t> SmbNtlmAuth::hmac_md5(const std::vector<uint8_t> & key, const std::vector<uint8_t> & data)
    {
        std::vector<uint8_t> result(16);
        unsigned int len = 16;
        HMAC(EVP_md5(), key.data(), static_cast<int>(key.size()), data.data(), data.size(), result.data(), &len);
        return result;
    }

    std::vector<uint8_t> SmbNtlmAuth::ntlmowfv1(const std::string & password)
    {
        auto utf16 = to_utf16le(password);
        return md4(utf16);
    }

    static std::optional<std::vector<uint8_t> > decode_hex_bytes(std::string_view text)
    {
        if ((text.size() % 2) != 0) {
            return std::nullopt;
        }
        auto hex_value = [](char ch)->int {
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
        };

        std::vector<uint8_t> out;
        out.reserve(text.size() / 2);
        for (size_t i = 0; i < text.size(); i += 2) {
            const int hi = hex_value(text[i]);
            const int lo = hex_value(text[i + 1]);
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            out.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        return out;
    }

    static std::optional<std::vector<uint8_t> > nt_hash_bytes_from_string(const std::string & text)
    {
        std::string_view value(text);
        constexpr std::string_view nthash_prefix = "nthash:";
        constexpr std::string_view ntlm_prefix = "ntlm:";
        if (value.rfind(nthash_prefix, 0) == 0) {
            value.remove_prefix(nthash_prefix.size());
        } else if (value.rfind(ntlm_prefix, 0) == 0) {
            value.remove_prefix(ntlm_prefix.size());
        }

        auto bytes = decode_hex_bytes(value);
        if (!bytes || bytes->size() != 16) {
            return std::nullopt;
        }
        return bytes;
    }

    static std::vector<uint8_t> ntlmowfv2_from_hash(const std::vector<uint8_t> & nt_hash,
                                                    const std::string & username,
                                                    const std::string & domain)
    {
        std::string upper_user = username;
        std::transform(upper_user.begin(), upper_user.end(), upper_user.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        std::string upper_domain = domain;
        std::transform(upper_domain.begin(), upper_domain.end(), upper_domain.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        auto identity = to_utf16le(upper_user + upper_domain);
        return SmbNtlmAuth::hmac_md5(nt_hash, identity);
    }

    static bool verify_ntlmv2_response_with_hash(const std::vector<uint8_t> & nt_hash,
                                                 const std::string & username,
                                                 const std::string & domain,
                                                 const std::array<uint8_t, 8> & server_challenge,
                                                 const std::vector<uint8_t> & ntlm_response)
    {
        if (ntlm_response.size() < 16) {
            return false;
        }
        const auto ntlmv2_hash = ntlmowfv2_from_hash(nt_hash, username, domain);
        std::vector<uint8_t> blob(ntlm_response.begin() + 16, ntlm_response.end());
        std::vector<uint8_t> temp(server_challenge.begin(), server_challenge.end());
        temp.insert(temp.end(), blob.begin(), blob.end());
        auto nt_proof = SmbNtlmAuth::hmac_md5(ntlmv2_hash, temp);
        std::vector<uint8_t> expected;
        expected.reserve(nt_proof.size() + blob.size());
        expected.insert(expected.end(), nt_proof.begin(), nt_proof.end());
        expected.insert(expected.end(), blob.begin(), blob.end());
        return constant_time_equal(expected, ntlm_response);
    }

    static std::vector<uint8_t> session_base_key_from_hash(const std::vector<uint8_t> & nt_hash,
                                                           const std::string & username,
                                                           const std::string & domain,
                                                           const std::vector<uint8_t> & ntlm_response)
    {
        if (ntlm_response.size() < 16) {
            return {};
        }
        auto ntlmv2_hash = ntlmowfv2_from_hash(nt_hash, username, domain);
        std::vector<uint8_t> nt_proof(ntlm_response.begin(), ntlm_response.begin() + 16);
        return SmbNtlmAuth::hmac_md5(ntlmv2_hash, nt_proof);
    }

    std::vector<uint8_t> SmbNtlmAuth::ntlmowfv2(const std::string & password, const std::string & username, const std::string & domain)
    {
        auto ntlm_hash = ntlmowfv1(password);
        std::string upper_user = username;
        std::transform(upper_user.begin(), upper_user.end(), upper_user.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        std::string upper_domain = domain;
        std::transform(upper_domain.begin(), upper_domain.end(), upper_domain.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        auto identity = to_utf16le(upper_user + upper_domain);
        return hmac_md5(ntlm_hash, identity);
    }

    std::vector<uint8_t> SmbNtlmAuth::ntlmv2_response(const std::string & password,
                                                      const std::string & username,
                                                      const std::string & domain,
                                                      const std::array<uint8_t, 8> & server_challenge,
                                                      const std::vector<uint8_t> & target_info)
    {
        auto ntlmv2_hash = ntlmowfv2(password, username, domain);
        std::vector<uint8_t> temp(server_challenge.begin(), server_challenge.end());
        temp.insert(temp.end(), target_info.begin(), target_info.end());
        auto nt_proof_str = hmac_md5(ntlmv2_hash, temp);
        std::vector<uint8_t> response;
        response.reserve(nt_proof_str.size() + target_info.size());
        response.insert(response.end(), nt_proof_str.begin(), nt_proof_str.end());
        response.insert(response.end(), target_info.begin(), target_info.end());
        return response;
    }

    bool SmbNtlmAuth::verify_ntlmv2_response(const std::string & password,
                                             const std::string & username,
                                             const std::string & domain,
                                             const std::array<uint8_t, 8> & server_challenge,
                                             const std::vector<uint8_t> & ntlm_response)
    {
        if (ntlm_response.size() < 16) {
            return false;
        }
        std::vector<uint8_t> blob(ntlm_response.begin() + 16, ntlm_response.end());
        auto expected = ntlmv2_response(password, username, domain, server_challenge, blob);
        return constant_time_equal(expected, ntlm_response);
    }

    std::vector<uint8_t> SmbNtlmAuth::session_base_key(const std::string & password,
                                                       const std::string & username,
                                                       const std::string & domain,
                                                       const std::vector<uint8_t> & ntlm_response)
    {
        if (ntlm_response.size() < 16) {
            return {};
        }
        auto ntlmv2_hash = ntlmowfv2(password, username, domain);
        std::vector<uint8_t> nt_proof(ntlm_response.begin(), ntlm_response.begin() + 16);
        return hmac_md5(ntlmv2_hash, nt_proof);
    }

    std::optional<NtlmType1Message> SmbNtlmAuth::parse_type1(const uint8_t * data, size_t len)
    {
        if (len < 16)
            return std::nullopt;
        if (std::memcmp(data, "NTLMSSP\0", 8) != 0)
            return std::nullopt;
        uint32_t type = read_u32_le(data + 8);
        if (type != NTLMSSP_MESSAGE_TYPE_NEGOTIATE)
            return std::nullopt;

        NtlmType1Message msg;
        msg.flags = read_u32_le(data + 12);
        msg.raw.assign(data, data + len);

        if (len >= 24) {
            uint16_t domain_len = read_u16_le(data + 16);
            uint32_t domain_offset = read_u32_le(data + 20);
            if (domain_offset + domain_len <= len) {
                msg.domain_name = from_utf16le(data + domain_offset, domain_len);
            }
        }

        if (len >= 32) {
            uint16_t ws_len = read_u16_le(data + 24);
            uint32_t ws_offset = read_u32_le(data + 28);
            if (ws_offset + ws_len <= len) {
                msg.workstation = from_utf16le(data + ws_offset, ws_len);
            }
        }

        return msg;
    }

    std::optional<NtlmType3Message> SmbNtlmAuth::parse_type3(const uint8_t * data, size_t len)
    {
        if (len < 64)
            return std::nullopt;
        if (std::memcmp(data, "NTLMSSP\0", 8) != 0)
            return std::nullopt;
        uint32_t type = read_u32_le(data + 8);
        if (type != NTLMSSP_MESSAGE_TYPE_AUTHENTICATE)
            return std::nullopt;

        NtlmType3Message msg;
        msg.flags = read_u32_le(data + 60);

        uint16_t lm_len = read_u16_le(data + 12);
        uint32_t lm_offset = read_u32_le(data + 16);
        if (lm_offset + lm_len <= len) {
            msg.lm_response.assign(data + lm_offset, data + lm_offset + lm_len);
        }

        uint16_t nt_len = read_u16_le(data + 20);
        uint32_t nt_offset = read_u32_le(data + 24);
        if (nt_offset + nt_len <= len) {
            msg.ntlm_response.assign(data + nt_offset, data + nt_offset + nt_len);
        }

        uint16_t dom_len = read_u16_le(data + 28);
        uint32_t dom_offset = read_u32_le(data + 32);
        if (dom_offset + dom_len <= len) {
            msg.domain_name = from_utf16le(data + dom_offset, dom_len);
        }

        uint16_t name_len = read_u16_le(data + 36);
        uint32_t name_offset = read_u32_le(data + 40);
        if (name_offset + name_len <= len) {
            msg.user_name = from_utf16le(data + name_offset, name_len);
        }

        uint16_t ws_len = read_u16_le(data + 44);
        uint32_t ws_offset = read_u32_le(data + 48);
        if (ws_offset + ws_len <= len) {
            msg.workstation = from_utf16le(data + ws_offset, ws_len);
        }

        if (len >= 64) {
            uint16_t key_len = read_u16_le(data + 52);
            uint32_t key_offset = read_u32_le(data + 56);
            if (key_offset + key_len <= len) {
                msg.encrypted_session_key.assign(data + key_offset, data + key_offset + key_len);
            }
        }

        return msg;
    }

    std::vector<uint8_t> SmbNtlmAuth::build_type2(const std::array<uint8_t, 8> & server_challenge,
                                                  const std::string & target_name,
                                                  const std::vector<uint8_t> & target_info,
                                                  uint32_t flags)
    {
        auto target_name_utf16 = to_utf16le(target_name);
        uint32_t target_name_offset = 48;
        uint32_t target_info_offset = target_name_offset + static_cast<uint32_t>(target_name_utf16.size());
        if (target_info_offset % 2 != 0)
            target_info_offset++;
        uint32_t total_len = target_info_offset + static_cast<uint32_t>(target_info.size());

        std::vector<uint8_t> buf(total_len, 0);
        std::memcpy(buf.data(), "NTLMSSP\0", 8);
        write_u32_le(buf.data() + 8, NTLMSSP_MESSAGE_TYPE_CHALLENGE);
        write_u16_le(buf.data() + 12, static_cast<uint16_t>(target_name_utf16.size()));
        write_u16_le(buf.data() + 14, static_cast<uint16_t>(target_name_utf16.size()));
        write_u32_le(buf.data() + 16, target_name_offset);
        write_u32_le(buf.data() + 20, flags);
        std::memcpy(buf.data() + 24, server_challenge.data(), 8);
        write_u16_le(buf.data() + 40, static_cast<uint16_t>(target_info.size()));
        write_u16_le(buf.data() + 42, static_cast<uint16_t>(target_info.size()));
        write_u32_le(buf.data() + 44, target_info_offset);

        std::memcpy(buf.data() + target_name_offset, target_name_utf16.data(), target_name_utf16.size());
        std::memcpy(buf.data() + target_info_offset, target_info.data(), target_info.size());

        return buf;
    }

    std::vector<uint8_t> SmbNtlmAuth::process_inbound_token(const std::vector<uint8_t> & token)
    {
        if (token.size() < 8)
            return {};
        if (std::memcmp(token.data(), "NTLMSSP\0", 8) != 0)
            return {};

        uint32_t type = read_u32_le(token.data() + 8);

        if (type == NTLMSSP_MESSAGE_TYPE_NEGOTIATE) {
            auto msg = parse_type1(token.data(), token.size());
            if (!msg) {
                state_ = AuthState::failed;
                return {};
            }

            negotiate_flags_ &= msg->flags;
            negotiate_flags_ |= NTLMSSP_NEGOTIATE_TARGET_INFO | NTLMSSP_TARGET_TYPE_SERVER | NTLMSSP_REQUEST_TARGET;
            server_challenge_ = generate_server_challenge();

            std::vector<uint8_t> target_info;
            auto add_av = [&](uint16_t id, const std::string &val) {
                auto utf16 = to_utf16le(val);
                target_info.resize(target_info.size() + 2);
                write_u16_le(target_info.data() + target_info.size() - 2, id);
                target_info.resize(target_info.size() + 2);
                write_u16_le(target_info.data() + target_info.size() - 2, static_cast<uint16_t>(utf16.size()));
                target_info.insert(target_info.end(), utf16.begin(), utf16.end());
            };
            add_av(NTLMSSP_AV_HOSTNAME, server_name_);
            add_av(NTLMSSP_AV_DOMAINNAME, domain_name_);
            add_av(NTLMSSP_AV_DNS_HOSTNAME, server_name_);
            add_av(NTLMSSP_AV_DNS_DOMAINNAME, domain_name_);
            target_info.resize(target_info.size() + 2);
            write_u16_le(target_info.data() + target_info.size() - 2, NTLMSSP_AV_EOL);
            target_info.resize(target_info.size() + 2);
            write_u16_le(target_info.data() + target_info.size() - 2, 0);

            auto type2 = build_type2(server_challenge_, domain_name_, target_info, negotiate_flags_);
            state_ = AuthState::challenge_sent;
            return type2;
        }

        if (type == NTLMSSP_MESSAGE_TYPE_AUTHENTICATE) {
            auto msg = parse_type3(token.data(), token.size());
            if (!msg) {
                state_ = AuthState::failed;
                result_.success = false;
                return {};
            }

            bool valid = false;
            std::optional<std::string> password;
            std::optional<std::vector<uint8_t> > nt_hash;
            if (password_lookup_) {
                password = password_lookup_(msg->user_name, msg->domain_name);
                if (password) {
                    valid = verify_ntlmv2_response(*password, msg->user_name, msg->domain_name,
                                                   server_challenge_, msg->ntlm_response);
                    if (valid && credential_validator_) {
                        valid = credential_validator_(msg->user_name, msg->domain_name, *password);
                    }
                }
            }
            if (!valid && nt_hash_lookup_) {
                auto encoded_hash = nt_hash_lookup_(msg->user_name, msg->domain_name);
                if (encoded_hash) {
                    nt_hash = nt_hash_bytes_from_string(*encoded_hash);
                    if (nt_hash) {
                        valid = verify_ntlmv2_response_with_hash(*nt_hash, msg->user_name, msg->domain_name,
                                                                 server_challenge_, msg->ntlm_response);
                        if (valid && credential_validator_) {
                            valid = credential_validator_(msg->user_name, msg->domain_name, "");
                        }
                    }
                }
            }
            if (!valid && !password && credential_validator_) {
                valid = credential_validator_(msg->user_name, msg->domain_name, "");
            }

             if (valid) {
                 state_ = AuthState::authenticated;
                 result_.success = true;
                 result_.user_name = msg->user_name;
                 result_.domain_name = msg->domain_name;

                 std::vector<uint8_t> base_key;
                 if (password) {
                     base_key = session_base_key(*password, msg->user_name,
                                                 msg->domain_name, msg->ntlm_response);
                 } else if (nt_hash) {
                     base_key = session_base_key_from_hash(*nt_hash, msg->user_name,
                                                           msg->domain_name, msg->ntlm_response);
                 }

                 if (!msg->encrypted_session_key.empty() && !base_key.empty()) {
                     result_.session_key = rc4_decrypt(base_key, msg->encrypted_session_key);
                 } else {
                     result_.session_key = std::move(base_key);
                 }
             } else {
                 state_ = AuthState::failed;
                 result_.success = false;
             }

             return {};
         }

         state_ = AuthState::failed;
         return {};
     }
 }
