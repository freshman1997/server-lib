#include "auth/smb_ntlm.h"
#include "openssl/evp.h"
#include "openssl/hmac.h"
#include "openssl/md4.h"
#include "openssl/rand.h"
#include <cstring>
#include <algorithm>

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

    std::array<uint8_t, 8> SmbNtlmAuth::generate_server_challenge()
    {
        std::array<uint8_t, 8> challenge{};
        RAND_bytes(challenge.data(), 8);
        return challenge;
    }

    std::vector<uint8_t> SmbNtlmAuth::md4(const std::vector<uint8_t> & data)
    {
        std::vector<uint8_t> digest(MD4_DIGEST_LENGTH);
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_md4(), nullptr);
        EVP_DigestUpdate(ctx, data.data(), data.size());
        EVP_DigestFinal_ex(ctx, digest.data(), nullptr);
        EVP_MD_CTX_free(ctx);
        return digest;
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

    std::vector<uint8_t> SmbNtlmAuth::ntlmowfv2(const std::string & password, const std::string & username, const std::string & domain)
    {
        auto ntlm_hash = ntlmowfv1(password);
        std::string upper_user = username;
        std::transform(upper_user.begin(), upper_user.end(), upper_user.begin(), ::toupper);
        std::string upper_domain = domain;
        std::transform(upper_domain.begin(), upper_domain.end(), upper_domain.begin(), ::toupper);
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

        uint16_t name_len = read_u16_le(data + 28);
        uint32_t name_offset = read_u32_le(data + 32);
        if (name_offset + name_len <= len) {
            msg.user_name = from_utf16le(data + name_offset, name_len);
        }

        uint16_t dom_len = read_u16_le(data + 36);
        uint32_t dom_offset = read_u32_le(data + 40);
        if (dom_offset + dom_len <= len) {
            msg.domain_name = from_utf16le(data + dom_offset, dom_len);
        }

        uint16_t ws_len = read_u16_le(data + 44);
        uint32_t ws_offset = read_u32_le(data + 48);
        if (ws_offset + ws_len <= len) {
            msg.workstation = from_utf16le(data + ws_offset, ws_len);
        }

        return msg;
    }

    std::vector<uint8_t> SmbNtlmAuth::build_type2(const std::array<uint8_t, 8> & server_challenge,
                                                  const std::string & target_name,
                                                  const std::vector<uint8_t> & target_info,
                                                  uint32_t flags)
    {
        auto target_name_utf16 = to_utf16le(target_name);
        uint32_t target_name_offset = 32 + 8;
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
            server_challenge_ = generate_server_challenge();

            std::vector<uint8_t> target_info;
            auto add_av = [&](uint16_t id, const std::string &val) {
                auto utf16 = to_utf16le(val);
                write_u16_le(target_info.data() + target_info.size(), id);
                target_info.resize(target_info.size() + 2);
                write_u16_le(target_info.data() + target_info.size(), static_cast<uint16_t>(utf16.size()));
                target_info.resize(target_info.size() + 2);
                target_info.insert(target_info.end(), utf16.begin(), utf16.end());
            };
            add_av(NTLMSSP_AV_HOSTNAME, server_name_);
            add_av(NTLMSSP_AV_DOMAINNAME, domain_name_);
            write_u16_le(target_info.data() + target_info.size(), NTLMSSP_AV_EOL);
            target_info.resize(target_info.size() + 2);
            write_u16_le(target_info.data() + target_info.size(), 0);
            target_info.resize(target_info.size() + 2);

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
            if (credential_validator_) {
                valid = credential_validator_(msg->user_name, msg->domain_name, "");
            }

            if (valid) {
                state_ = AuthState::authenticated;
                result_.success = true;
                result_.user_name = msg->user_name;
                result_.domain_name = msg->domain_name;

                if (msg->ntlm_response.size() >= 16) {
                    auto ntlmv2_hash = ntlmowfv2("", msg->user_name, msg->domain_name);
                    std::vector<uint8_t> temp(server_challenge_.begin(), server_challenge_.end());
                    temp.insert(temp.end(), msg->ntlm_response.begin() + 16, msg->ntlm_response.end());
                    auto session_base = hmac_md5(ntlmv2_hash, temp);
                    result_.session_key = hmac_md5(session_base, std::vector<uint8_t>{});
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
