#include "auth/ssh_auth_publickey.h"
#include "protocol/ssh_message_codec.h"
#include "ssh_session.h"
#include "net/connection/connection.h"
#include "net/socket/inet_address.h"

#include "openssl/bn.h"
#include "openssl/core_names.h"
#include "openssl/evp.h"
#include "openssl/params.h"
#include "openssl/rsa.h"
#include "openssl/x509.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace yuan::net::ssh
{
    namespace
    {
        std::string trim_copy(const std::string &value)
        {
            size_t begin = 0;
            while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
                ++begin;
            }

            size_t end = value.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
                --end;
            }
            return value.substr(begin, end - begin);
        }

        std::vector<std::string> split_ws(const std::string &line)
        {
            std::vector<std::string> parts;
            std::istringstream iss(line);
            std::string token;
            while (iss >> token) {
                parts.push_back(std::move(token));
            }
            return parts;
        }

        std::vector<std::string> split_csv_respecting_quotes(const std::string &value)
        {
            std::vector<std::string> items;
            std::string current;
            bool in_quotes = false;
            for (char ch : value) {
                if (ch == '"') {
                    in_quotes = !in_quotes;
                    current.push_back(ch);
                    continue;
                }
                if (ch == ',' && !in_quotes) {
                    items.push_back(trim_copy(current));
                    current.clear();
                    continue;
                }
                current.push_back(ch);
            }
            if (!current.empty()) {
                items.push_back(trim_copy(current));
            }
            return items;
        }

        bool is_key_type_token(const std::string &token)
        {
            return token.rfind("ssh-", 0) == 0 ||
                   token.rfind("ecdsa-sha2-", 0) == 0 ||
                   token.rfind("sk-", 0) == 0;
        }

        std::vector<uint8_t> decode_base64(const std::string &input)
        {
            if (input.empty()) {
                return {};
            }

            std::string normalized;
            normalized.reserve(input.size() + 3);
            for (char ch : input) {
                if (std::isspace(static_cast<unsigned char>(ch)) == 0) {
                    normalized.push_back(ch);
                }
            }

            if (normalized.empty()) {
                return {};
            }

            const size_t remainder = normalized.size() % 4;
            if (remainder != 0) {
                normalized.append(4 - remainder, '=');
            }

            std::vector<uint8_t> output((normalized.size() / 4) * 3, 0);
            const int decoded_len = EVP_DecodeBlock(
                output.data(),
                reinterpret_cast<const unsigned char *>(normalized.data()),
                static_cast<int>(normalized.size()));
            if (decoded_len < 0) {
                return {};
            }

            int padding = 0;
            if (!normalized.empty() && normalized.back() == '=') {
                ++padding;
            }
            if (normalized.size() >= 2 && normalized[normalized.size() - 2] == '=') {
                ++padding;
            }

            output.resize(static_cast<size_t>(decoded_len - padding));
            return output;
        }

        std::filesystem::path resolve_authorized_keys_path()
        {
            const char *override_path = std::getenv("YUAN_SSH_AUTHORIZED_KEYS");
            if (override_path && *override_path != '\0') {
                return std::filesystem::path(override_path);
            }

            const char *home = std::getenv("HOME");
            if (home && *home != '\0') {
                return std::filesystem::path(home) / ".ssh" / "authorized_keys";
            }

            return std::filesystem::path(".ssh/authorized_keys");
        }

        bool algorithm_matches_authorized_type(const std::string &auth_type,
                                               const std::string &algorithm)
        {
            if (auth_type == algorithm) {
                return true;
            }

            if (auth_type == "ssh-rsa" &&
                (algorithm == "ssh-rsa" || algorithm == "rsa-sha2-256" || algorithm == "rsa-sha2-512")) {
                return true;
            }

            return false;
        }

        bool wildcard_match(const std::string &pattern, const std::string &value)
        {
            size_t p = 0;
            size_t v = 0;
            size_t star = std::string::npos;
            size_t match = 0;

            while (v < value.size()) {
                if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) {
                    ++p;
                    ++v;
                } else if (p < pattern.size() && pattern[p] == '*') {
                    star = p++;
                    match = v;
                } else if (star != std::string::npos) {
                    p = star + 1;
                    v = ++match;
                } else {
                    return false;
                }
            }

            while (p < pattern.size() && pattern[p] == '*') {
                ++p;
            }
            return p == pattern.size();
        }

        std::string trim_quotes(const std::string &value)
        {
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                return value.substr(1, value.size() - 2);
            }
            return value;
        }

        std::string session_remote_ip(SshSession *session)
        {
            if (!session) {
                return {};
            }
            auto conn = session->client_connection();
            if (!conn) {
                return {};
            }
            return conn->get_remote_address().get_ip();
        }

        bool from_option_allows_remote(const std::string &patterns_csv,
                                       const std::string &remote_ip)
        {
            if (remote_ip.empty()) {
                return false;
            }

            const auto patterns = split_csv_respecting_quotes(patterns_csv);
            bool has_positive = false;
            bool positive_match = false;

            for (const auto &raw_pattern : patterns) {
                if (raw_pattern.empty()) {
                    continue;
                }
                bool negated = raw_pattern[0] == '!';
                const std::string pattern = negated ? raw_pattern.substr(1) : raw_pattern;
                if (pattern.empty()) {
                    continue;
                }
                const bool matched = wildcard_match(pattern, remote_ip);
                if (negated && matched) {
                    return false;
                }
                if (!negated) {
                    has_positive = true;
                    if (matched) {
                        positive_match = true;
                    }
                }
            }

            return has_positive ? positive_match : true;
        }

        struct AuthorizedKeyRestrictions
        {
            bool no_pty = false;
            bool no_port_forwarding = false;
            bool no_agent_forwarding = false;
            bool no_x11_forwarding = false;
            std::string forced_command;
            std::vector<std::string> permitopen;
            std::vector<std::string> permitlisten;
        };

        bool key_is_authorized(const std::string &algorithm,
                               const std::vector<uint8_t> &public_key_blob,
                               SshSession *session,
                               AuthorizedKeyRestrictions &restrictions_out)
        {
            const auto authorized_keys_path = resolve_authorized_keys_path();
            std::ifstream in(authorized_keys_path);
            if (!in.good()) {
                return false;
            }

            std::string line;
            while (std::getline(in, line)) {
                const std::string trimmed = trim_copy(line);
                if (trimmed.empty() || trimmed[0] == '#') {
                    continue;
                }

                const auto parts = split_ws(trimmed);
                if (parts.size() < 2) {
                    continue;
                }

                size_t key_type_index = 0;
                if (!is_key_type_token(parts[0])) {
                    if (parts.size() < 3 || !is_key_type_token(parts[1])) {
                        continue;
                    }
                    key_type_index = 1;
                }

                const std::string &auth_type = parts[key_type_index];
                const std::string &auth_key_b64 = parts[key_type_index + 1];
                if (!algorithm_matches_authorized_type(auth_type, algorithm)) {
                    continue;
                }

                const auto decoded = decode_base64(auth_key_b64);
                if (decoded.empty()) {
                    continue;
                }

                if (decoded == public_key_blob) {
                    AuthorizedKeyRestrictions restrictions;
                    if (key_type_index > 0) {
                        const auto options = split_csv_respecting_quotes(parts[0]);
                        for (const auto &opt_raw : options) {
                            const auto opt = trim_copy(opt_raw);
                            constexpr std::string_view from_prefix = "from=";
                            constexpr std::string_view command_prefix = "command=";
                            constexpr std::string_view permitopen_prefix = "permitopen=";
                            constexpr std::string_view permitlisten_prefix = "permitlisten=";
                            if (opt.rfind(from_prefix, 0) == 0) {
                                const std::string patterns = trim_quotes(opt.substr(from_prefix.size()));
                                if (!from_option_allows_remote(patterns, session_remote_ip(session))) {
                                    goto continue_scan;
                                }
                            } else if (opt == "restrict") {
                                restrictions.no_pty = true;
                                restrictions.no_port_forwarding = true;
                                restrictions.no_agent_forwarding = true;
                                restrictions.no_x11_forwarding = true;
                            } else if (opt == "no-pty") {
                                restrictions.no_pty = true;
                            } else if (opt == "no-port-forwarding") {
                                restrictions.no_port_forwarding = true;
                            } else if (opt == "no-agent-forwarding") {
                                restrictions.no_agent_forwarding = true;
                            } else if (opt == "no-x11-forwarding") {
                                restrictions.no_x11_forwarding = true;
                            } else if (opt == "pty") {
                                restrictions.no_pty = false;
                            } else if (opt == "port-forwarding") {
                                restrictions.no_port_forwarding = false;
                            } else if (opt == "agent-forwarding") {
                                restrictions.no_agent_forwarding = false;
                            } else if (opt == "x11-forwarding") {
                                restrictions.no_x11_forwarding = false;
                            } else if (opt.rfind(command_prefix, 0) == 0) {
                                restrictions.forced_command = trim_quotes(opt.substr(command_prefix.size()));
                            } else if (opt.rfind(permitopen_prefix, 0) == 0) {
                                restrictions.permitopen.push_back(trim_quotes(opt.substr(permitopen_prefix.size())));
                            } else if (opt.rfind(permitlisten_prefix, 0) == 0) {
                                restrictions.permitlisten.push_back(trim_quotes(opt.substr(permitlisten_prefix.size())));
                            }
                        }
                    }
                    restrictions_out = std::move(restrictions);
                    return true;
                }

            continue_scan:
                continue;
            }

            return false;
        }

        std::vector<uint8_t> encode_public_key_der(EVP_PKEY * pkey)
        {
            if (!pkey) {
                return {};
            }

            uint8_t * der = nullptr;
            const int der_len = i2d_PUBKEY(pkey, &der);
            if (der_len <= 0 || !der) {
                return {};
            }

            std::vector<uint8_t> result(der, der + der_len);
            OPENSSL_free(der);
            return result;
        }

        bool verify_rsa_signature_from_blob(const std::vector<uint8_t> & public_key_blob,
                                            const std::string & algorithm,
                                            const uint8_t * data,
                                            size_t data_len,
                                            const uint8_t * signature,
                                            size_t signature_len)
        {
            size_t offset = 0;
            auto key_type = SshMessageCodec::read_string(public_key_blob.data(), public_key_blob.size(), offset);
            auto exponent = SshMessageCodec::read_mpint(public_key_blob.data(), public_key_blob.size(), offset);
            auto modulus = SshMessageCodec::read_mpint(public_key_blob.data(), public_key_blob.size(), offset);
            if (!key_type || *key_type != "ssh-rsa" || !exponent || !modulus) {
                return false;
            }

            BIGNUM * e = BN_bin2bn(exponent->data(), static_cast<int>(exponent->size()), nullptr);
            BIGNUM * n = BN_bin2bn(modulus->data(), static_cast<int>(modulus->size()), nullptr);
            if (!e || !n) {
                BN_free(e);
                BN_free(n);
                return false;
            }

            const EVP_MD * md = nullptr;
            if (algorithm == "rsa-sha2-512") {
                md = EVP_sha512();
            } else if (algorithm == "rsa-sha2-256") {
                md = EVP_sha256();
            } else if (algorithm == "ssh-rsa") {
                md = EVP_sha1();
            } else {
                BN_free(e);
                BN_free(n);
                return false;
            }

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
            RSA * rsa = RSA_new();
            EVP_PKEY * pkey = nullptr;
            EVP_MD_CTX * md_ctx = nullptr;
            bool verified = false;
            if (rsa && RSA_set0_key(rsa, n, e, nullptr) == 1) {
                pkey = EVP_PKEY_new();
                if (pkey && EVP_PKEY_assign_RSA(pkey, rsa) == 1) {
                    rsa = nullptr;
                    md_ctx = EVP_MD_CTX_new();
                    if (md_ctx && EVP_DigestVerifyInit(md_ctx, nullptr, md, nullptr, pkey) == 1) {
                        verified = EVP_DigestVerify(md_ctx, signature, signature_len, data, data_len) == 1;
                    }
                }
            }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

            EVP_MD_CTX_free(md_ctx);
            EVP_PKEY_free(pkey);
            if (rsa) {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
                RSA_free(rsa);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
                BN_free(e);
                BN_free(n);
            }
            return verified;
        }

        std::vector<uint8_t> decode_ecdsa_public_key_der(const std::vector<uint8_t> & public_key_blob,
                                                          const std::string & expected_algorithm,
                                                          std::string & curve_out)
        {
            size_t offset = 0;
            auto algorithm = SshMessageCodec::read_string(public_key_blob.data(), public_key_blob.size(), offset);
            auto curve_name = SshMessageCodec::read_string(public_key_blob.data(), public_key_blob.size(), offset);
            auto public_point = SshMessageCodec::read_string(public_key_blob.data(), public_key_blob.size(), offset);
            if (!algorithm || *algorithm != expected_algorithm || !curve_name || !public_point) {
                return {};
            }

            const char * group_name = nullptr;
            if (*curve_name == "nistp256") {
                group_name = "prime256v1";
                curve_out = "P-256";
            } else if (*curve_name == "nistp384") {
                group_name = "secp384r1";
                curve_out = "P-384";
            } else if (*curve_name == "nistp521") {
                group_name = "secp521r1";
                curve_out = "P-521";
            } else {
                return {};
            }

            OSSL_PARAM params[3];
            params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                         const_cast<char *>(group_name), 0);
            params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
                                                          const_cast<char *>(public_point->data()),
                                                          public_point->size());
            params[2] = OSSL_PARAM_construct_end();

            EVP_PKEY_CTX * ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
            EVP_PKEY * pkey = nullptr;
            std::vector<uint8_t> der;
            if (ctx && EVP_PKEY_fromdata_init(ctx) > 0 &&
                EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) > 0) {
                der = encode_public_key_der(pkey);
            }

            EVP_PKEY_free(pkey);
            EVP_PKEY_CTX_free(ctx);
            return der;
        }
    }

    SshAuthPublickey::SshAuthPublickey(SshCrypto * crypto)
        : crypto_(crypto)
    {
    }

    SshAuthResult SshAuthPublickey::authenticate(SshSession * session,
                                                 const std::string & username,
                                                 const SshAuthCredentials & credentials)
    {
        (void)username;

        if (credentials.public_key_blob.empty()) {
            return SshAuthResult::FAILURE;
        }

        AuthorizedKeyRestrictions restrictions;
        if (!key_is_authorized(credentials.public_key_algorithm,
                               credentials.public_key_blob,
                               session,
                               restrictions)) {
            return SshAuthResult::FAILURE;
        }

        if (session) {
            session->set_authorized_key_restrictions(restrictions.no_pty,
                                                     restrictions.forced_command,
                                                     restrictions.no_port_forwarding,
                                                     restrictions.no_agent_forwarding,
                                                     restrictions.no_x11_forwarding,
                                                     restrictions.permitopen,
                                                     restrictions.permitlisten);
        }

        if (!credentials.has_signature)
            return SshAuthResult::NEED_MORE;

        if (credentials.signature.empty() || credentials.public_key_blob.empty())
            return SshAuthResult::FAILURE;

        if (!session)
            return SshAuthResult::FAILURE;

        const auto &session_id = session->session_id_proto();
        return verify_signature(session_id, username,
                                credentials.public_key_algorithm,
                                credentials.public_key_blob,
                                credentials.signature)
                   ? SshAuthResult::SUCCESS
                   : SshAuthResult::FAILURE;
    }

    bool SshAuthPublickey::verify_signature(const std::vector<uint8_t> & session_id,
                                            const std::string & username,
                                            const std::string & algorithm,
                                            const std::vector<uint8_t> & public_key_blob,
                                            const std::vector<uint8_t> & signature)
    {
        if (!crypto_)
            return false;

        ByteBuffer signed_data;
        SshMessageCodec::write_raw(signed_data, session_id.data(), session_id.size());
        signed_data.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_REQUEST));
        SshMessageCodec::write_string(signed_data, username);
        SshMessageCodec::write_string(signed_data, SSH_SERVICE_CONNECTION);
        SshMessageCodec::write_string(signed_data, "publickey");
        SshMessageCodec::write_boolean(signed_data, true);
        SshMessageCodec::write_string(signed_data, algorithm);
        SshMessageCodec::write_raw(signed_data, public_key_blob.data(), public_key_blob.size());

        size_t sig_offset = 0;
        auto signature_blob = SshMessageCodec::read_string(signature.data(), signature.size(), sig_offset);
        if (!signature_blob) {
            return false;
        }

        sig_offset = 0;
        auto signature_algorithm = SshMessageCodec::read_string(
            reinterpret_cast<const uint8_t *>(signature_blob->data()), signature_blob->size(), sig_offset);
        if (!signature_algorithm || *signature_algorithm != algorithm) {
            return false;
        }

        auto signature_value = SshMessageCodec::read_string(
            reinterpret_cast<const uint8_t *>(signature_blob->data()), signature_blob->size(), sig_offset);
        if (!signature_value) {
            return false;
        }

        size_t key_offset = 0;
        auto key_algorithm = SshMessageCodec::read_string(public_key_blob.data(), public_key_blob.size(), key_offset);
        if (!key_algorithm) {
            return false;
        }

        const bool rsa_algorithm_match =
            (*key_algorithm == "ssh-rsa") &&
            (algorithm == "ssh-rsa" || algorithm == "rsa-sha2-256" || algorithm == "rsa-sha2-512");
        if (*key_algorithm != algorithm && !rsa_algorithm_match) {
            return false;
        }

        const auto * signed_ptr = reinterpret_cast<const uint8_t *>(signed_data.read_ptr());
        const auto signed_len = signed_data.readable_bytes();
        const auto * sig_ptr = reinterpret_cast<const uint8_t *>(signature_value->data());
        const auto sig_len = signature_value->size();

        if (algorithm == "ssh-ed25519") {
            auto raw_public_key = SshMessageCodec::read_string(public_key_blob.data(), public_key_blob.size(), key_offset);
            if (!raw_public_key) {
                return false;
            }
            return crypto_->ed25519_verify(
                std::vector<uint8_t>(raw_public_key->begin(), raw_public_key->end()),
                signed_ptr, signed_len, sig_ptr, sig_len);
        }

        if (algorithm == "ssh-rsa" || algorithm == "rsa-sha2-256" || algorithm == "rsa-sha2-512") {
            return verify_rsa_signature_from_blob(public_key_blob, algorithm, signed_ptr, signed_len, sig_ptr, sig_len);
        }

        if (algorithm.rfind("ecdsa-sha2-", 0) == 0) {
            std::string curve;
            auto ecdsa_public_key_der = decode_ecdsa_public_key_der(public_key_blob, algorithm, curve);
            if (ecdsa_public_key_der.empty() || curve.empty()) {
                return false;
            }

            return crypto_->ecdsa_verify(ecdsa_public_key_der, curve, signed_ptr, signed_len, sig_ptr, sig_len);
        }

        return false;
    }
}
