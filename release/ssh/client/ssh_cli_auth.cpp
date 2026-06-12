#include "ssh_cli_auth.h"

#include "base/utils/base64.h"
#include "buffer/byte_buffer.h"
#include "protocol/ssh_message_codec.h"

#include "openssl/bn.h"
#include "openssl/core_names.h"
#include "openssl/evp.h"
#include "openssl/pem.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace yuan::release_ssh::client
{
    namespace
    {
        std::optional<std::vector<uint8_t>> load_file_bytes(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in.good()) {
                return std::nullopt;
            }
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return data;
        }

        std::vector<uint8_t> buffer_to_bytes(const yuan::buffer::ByteBuffer &buf)
        {
            auto span = buf.readable_span();
            return std::vector<uint8_t>(
                reinterpret_cast<const uint8_t *>(span.data()),
                reinterpret_cast<const uint8_t *>(span.data()) + span.size());
        }
    }

    bool check_known_hosts(const CliArgs &args,
                           const std::vector<uint8_t> &host_key_blob,
                           std::string &error_message)
    {
        size_t offset = 0;
        auto host_key_algorithm = yuan::net::ssh::SshMessageCodec::read_string(
            host_key_blob.data(), host_key_blob.size(), offset);
        if (!host_key_algorithm) {
            error_message = "invalid server host key blob";
            return false;
        }

        const std::string host_key_b64 = yuan::base::util::base64_encode(host_key_blob);
        const std::string host_token =
            args.port == 22 ? args.host : ("[" + args.host + "]:" + std::to_string(args.port));
        const std::string host_plain = args.host;

        const std::filesystem::path known_hosts_path =
            args.known_hosts_file.empty() ? std::filesystem::path(default_known_hosts_file())
                                          : std::filesystem::path(args.known_hosts_file);

        bool found_exact = false;
        bool found_conflict = false;
        std::ifstream in(known_hosts_path);
        std::string line;
        while (std::getline(in, line)) {
            const std::string trimmed = trim_copy(line);
            if (trimmed.empty() || trimmed[0] == '#') {
                continue;
            }

            const auto parts = split_ws(trimmed);
            if (parts.size() < 3) {
                continue;
            }

            size_t host_index = 0;
            size_t algo_index = 1;
            size_t key_index = 2;
            if (!parts.empty() && !parts[0].empty() && parts[0][0] == '@') {
                if (parts.size() < 4) {
                    continue;
                }
                host_index = 1;
                algo_index = 2;
                key_index = 3;
            }

            const std::string &hosts_field = parts[host_index];
            if (!hosts_field.empty() && hosts_field[0] == '|') {
                continue;
            }

            if (!host_matches_known_hosts_field(hosts_field, host_token, host_plain)) {
                continue;
            }

            if (parts[algo_index] == *host_key_algorithm && parts[key_index] == host_key_b64) {
                found_exact = true;
                break;
            }

            found_conflict = true;
        }

        if (found_conflict && args.host_key_policy != CliArgs::HostKeyPolicy::no) {
            error_message = "host key mismatch for " + host_token;
            return false;
        }

        if (found_exact) {
            return true;
        }

        if (args.host_key_policy == CliArgs::HostKeyPolicy::yes) {
            error_message = "host key not found in known_hosts for " + host_token;
            return false;
        }

        std::error_code ec;
        const auto parent = known_hosts_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }

        std::ofstream out(known_hosts_path, std::ios::app);
        if (!out.good()) {
            error_message = "failed to update known_hosts file: " + known_hosts_path.string();
            return false;
        }
        out << host_token << ' ' << *host_key_algorithm << ' ' << host_key_b64 << '\n';
        return true;
    }

    std::vector<std::string> split_name_list(const std::string &csv)
    {
        std::vector<std::string> names;
        size_t start = 0;
        while (start <= csv.size()) {
            const size_t comma = csv.find(',', start);
            if (comma == std::string::npos) {
                if (start < csv.size()) {
                    names.push_back(csv.substr(start));
                }
                break;
            }
            if (comma > start) {
                names.push_back(csv.substr(start, comma - start));
            }
            start = comma + 1;
        }
        return names;
    }

    bool list_contains(const std::vector<std::string> &names, std::string_view needle)
    {
        for (const auto &name : names) {
            if (name == needle) {
                return true;
            }
        }
        return false;
    }

    std::optional<std::vector<uint8_t>> load_private_key_der_any(const std::string &path)
    {
        auto raw = load_file_bytes(path);
        if (!raw || raw->empty()) {
            return std::nullopt;
        }

        if (raw->size() > 16 && (*raw)[0] == 0x30) {
            return raw;
        }

        BIO *bio = BIO_new_mem_buf(raw->data(), static_cast<int>(raw->size()));
        if (!bio) {
            return std::nullopt;
        }

        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) {
            return std::nullopt;
        }

        uint8_t *der = nullptr;
        const int der_len = i2d_PrivateKey(pkey, &der);
        EVP_PKEY_free(pkey);
        if (der_len <= 0 || !der) {
            return std::nullopt;
        }

        std::vector<uint8_t> der_vec(der, der + der_len);
        OPENSSL_free(der);
        return der_vec;
    }

    std::optional<std::vector<uint8_t>> build_public_key_blob_from_private_der(const std::vector<uint8_t> &private_key_der,
                                                                                std::string &algorithm_out)
    {
        const uint8_t *p = private_key_der.data();
        EVP_PKEY *pkey = d2i_AutoPrivateKey(nullptr, &p, static_cast<long>(private_key_der.size()));
        if (!pkey) {
            return std::nullopt;
        }

        std::optional<std::vector<uint8_t>> result;
        const int type = EVP_PKEY_base_id(pkey);
        if (type == EVP_PKEY_ED25519) {
            size_t pub_len = 32;
            std::vector<uint8_t> pub(pub_len);
            if (EVP_PKEY_get_octet_string_param(
                    pkey,
                    OSSL_PKEY_PARAM_PUB_KEY,
                    pub.data(),
                    pub_len,
                    &pub_len) == 1) {
                pub.resize(pub_len);
                yuan::buffer::ByteBuffer buf;
                yuan::net::ssh::SshMessageCodec::write_string(buf, "ssh-ed25519");
                yuan::net::ssh::SshMessageCodec::write_raw(buf, pub.data(), pub.size());
                algorithm_out = "ssh-ed25519";
                result = buffer_to_bytes(buf);
            }
        } else if (type == EVP_PKEY_RSA) {
            BIGNUM *n = nullptr;
            BIGNUM *e = nullptr;
            EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n);
            EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e);
            if (n && e) {
                std::vector<uint8_t> modulus(BN_num_bytes(n));
                std::vector<uint8_t> exponent(BN_num_bytes(e));
                BN_bn2bin(n, modulus.data());
                BN_bn2bin(e, exponent.data());

                yuan::buffer::ByteBuffer buf;
                yuan::net::ssh::SshMessageCodec::write_string(buf, "ssh-rsa");
                yuan::net::ssh::SshMessageCodec::write_mpint(buf, exponent);
                yuan::net::ssh::SshMessageCodec::write_mpint(buf, modulus);
                algorithm_out = "rsa-sha2-256";
                result = buffer_to_bytes(buf);
            }
            BN_free(n);
            BN_free(e);
        } else if (type == EVP_PKEY_EC) {
            size_t pub_len = 0;
            EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &pub_len);
            std::vector<uint8_t> pub(pub_len);
            if (pub_len > 0 &&
                EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY, pub.data(), pub_len, &pub_len) == 1) {
                pub.resize(pub_len);

                char group_name[32] = {};
                size_t group_len = sizeof(group_name);
                if (EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME, group_name, sizeof(group_name), &group_len) == 1) {
                    std::string curve;
                    std::string algorithm;
                    if (std::string(group_name) == "prime256v1") {
                        curve = "nistp256";
                        algorithm = "ecdsa-sha2-nistp256";
                    } else if (std::string(group_name) == "secp384r1") {
                        curve = "nistp384";
                        algorithm = "ecdsa-sha2-nistp384";
                    } else if (std::string(group_name) == "secp521r1") {
                        curve = "nistp521";
                        algorithm = "ecdsa-sha2-nistp521";
                    }

                    if (!curve.empty()) {
                        yuan::buffer::ByteBuffer buf;
                        yuan::net::ssh::SshMessageCodec::write_string(buf, algorithm);
                        yuan::net::ssh::SshMessageCodec::write_string(buf, curve);
                        yuan::net::ssh::SshMessageCodec::write_string(buf, std::string(
                            reinterpret_cast<const char *>(pub.data()),
                            pub.size()));
                        algorithm_out = algorithm;
                        result = buffer_to_bytes(buf);
                    }
                }
            }
        }

        EVP_PKEY_free(pkey);
        return result;
    }

    bool load_client_identity(const CliArgs &args, ClientIdentity &identity, std::ostream &err)
    {
        identity = ClientIdentity{};
        if (args.identity_files.empty()) {
            return true;
        }

        const auto maybe_der = load_private_key_der_any(args.identity_files.front());
        if (!maybe_der) {
            err << "failed to load private key: " << args.identity_files.front() << '\n';
            return false;
        }
        identity.private_key_der = *maybe_der;

        auto maybe_blob = build_public_key_blob_from_private_der(identity.private_key_der, identity.algorithm);
        if (!maybe_blob) {
            err << "failed to derive SSH public key blob from private key\n";
            return false;
        }
        identity.public_key_blob = *maybe_blob;
        return true;
    }

    std::vector<uint8_t> make_password_method_data(const std::string &password)
    {
        yuan::buffer::ByteBuffer method;
        yuan::net::ssh::SshMessageCodec::write_boolean(method, false);
        yuan::net::ssh::SshMessageCodec::write_string(method, password);
        return buffer_to_bytes(method);
    }

    std::vector<uint8_t> make_publickey_method_data(const std::string &algorithm,
                                                    const std::vector<uint8_t> &public_key_blob,
                                                    bool include_signature,
                                                    const std::vector<uint8_t> &signature)
    {
        yuan::buffer::ByteBuffer method;
        yuan::net::ssh::SshMessageCodec::write_boolean(method, include_signature);
        yuan::net::ssh::SshMessageCodec::write_string(method, algorithm);
        yuan::net::ssh::SshMessageCodec::write_raw(method, public_key_blob.data(), public_key_blob.size());
        if (include_signature) {
            yuan::net::ssh::SshMessageCodec::write_raw(method, signature.data(), signature.size());
        }
        return buffer_to_bytes(method);
    }

    std::optional<std::vector<uint8_t>> make_publickey_signature(const std::vector<uint8_t> &session_id,
                                                                 const std::string &username,
                                                                 const std::string &algorithm,
                                                                 const std::vector<uint8_t> &public_key_blob,
                                                                 const std::vector<uint8_t> &private_key_der,
                                                                 yuan::net::ssh::SshCryptoOpenSSL &crypto)
    {
        yuan::buffer::ByteBuffer signed_data;
        yuan::net::ssh::SshMessageCodec::write_raw(signed_data, session_id.data(), session_id.size());
        signed_data.append_u8(static_cast<uint8_t>(yuan::net::ssh::SshMessageType::SSH_MSG_USERAUTH_REQUEST));
        yuan::net::ssh::SshMessageCodec::write_string(signed_data, username);
        yuan::net::ssh::SshMessageCodec::write_string(signed_data, yuan::net::ssh::SSH_SERVICE_CONNECTION);
        yuan::net::ssh::SshMessageCodec::write_string(signed_data, "publickey");
        yuan::net::ssh::SshMessageCodec::write_boolean(signed_data, true);
        yuan::net::ssh::SshMessageCodec::write_string(signed_data, algorithm);
        yuan::net::ssh::SshMessageCodec::write_raw(signed_data, public_key_blob.data(), public_key_blob.size());

        auto signed_data_bytes = buffer_to_bytes(signed_data);
        std::vector<uint8_t> raw_sig;
        if (algorithm == "ssh-ed25519") {
            const uint8_t *p = private_key_der.data();
            EVP_PKEY *pkey = d2i_AutoPrivateKey(nullptr, &p, static_cast<long>(private_key_der.size()));
            if (!pkey) {
                return std::nullopt;
            }

            size_t priv_len = 32;
            std::vector<uint8_t> raw_priv(priv_len);
            if (EVP_PKEY_get_octet_string_param(
                    pkey,
                    OSSL_PKEY_PARAM_PRIV_KEY,
                    raw_priv.data(),
                    raw_priv.size(),
                    &priv_len) != 1) {
                EVP_PKEY_free(pkey);
                return std::nullopt;
            }
            raw_priv.resize(priv_len);
            EVP_PKEY_free(pkey);
            raw_sig = crypto.ed25519_sign(raw_priv, signed_data_bytes.data(), signed_data_bytes.size());
        } else if (algorithm == "rsa-sha2-256") {
            raw_sig = crypto.rsa_sign(private_key_der, "sha256", signed_data_bytes.data(), signed_data_bytes.size());
        } else if (algorithm == "rsa-sha2-512") {
            raw_sig = crypto.rsa_sign(private_key_der, "sha512", signed_data_bytes.data(), signed_data_bytes.size());
        } else if (algorithm == "ecdsa-sha2-nistp256") {
            raw_sig = crypto.ecdsa_sign(private_key_der, "P-256", signed_data_bytes.data(), signed_data_bytes.size());
        } else if (algorithm == "ecdsa-sha2-nistp384") {
            raw_sig = crypto.ecdsa_sign(private_key_der, "P-384", signed_data_bytes.data(), signed_data_bytes.size());
        } else if (algorithm == "ecdsa-sha2-nistp521") {
            raw_sig = crypto.ecdsa_sign(private_key_der, "P-521", signed_data_bytes.data(), signed_data_bytes.size());
        }

        if (raw_sig.empty()) {
            return std::nullopt;
        }

        yuan::buffer::ByteBuffer signature_field;
        yuan::net::ssh::SshMessageCodec::write_string(signature_field, algorithm);
        yuan::net::ssh::SshMessageCodec::write_string(signature_field, std::string(
            reinterpret_cast<const char *>(raw_sig.data()),
            raw_sig.size()));
        return buffer_to_bytes(signature_field);
    }
}
