#include "transport/ssh_transport.h"
#include "protocol/ssh_message_codec.h"
#include "crypto/ssh_key_derivation.h"
#include "ssh_config.h"
#include <algorithm>
#include <cstring>
#include "openssl/core_names.h"
#include "openssl/evp.h"
#include "openssl/params.h"
#include "openssl/x509.h"
#include <string_view>

namespace yuan::net::ssh
{
    namespace
    {
        std::string first_name_in_list(const std::string & names)
        {
            const auto pos = names.find(',');
            if (pos == std::string::npos) {
                return names;
            }
            return names.substr(0, pos);
        }

        std::vector<std::string> filter_supported_algorithms(
            const std::vector<std::string> & preferred,
            const std::vector<std::string> & supported)
        {
            if (supported.empty()) {
                return preferred;
            }

            std::vector<std::string> filtered;
            filtered.reserve(preferred.size());
            for (const auto & name : preferred) {
                if (std::find(supported.begin(), supported.end(), name) != supported.end()) {
                    filtered.push_back(name);
                }
            }
            return filtered;
        }

        std::vector<uint8_t> to_bytes(const std::string & value)
        {
            return std::vector<uint8_t>(
                reinterpret_cast<const uint8_t *>(value.data()),
                reinterpret_cast<const uint8_t *>(value.data()) + value.size());
        }

        std::vector<std::string> split_name_list(const std::string & names)
        {
            std::vector<std::string> out;
            size_t start = 0;
            while (start <= names.size()) {
                size_t comma = names.find(',', start);
                if (comma == std::string::npos) {
                    comma = names.size();
                }
                if (comma > start) {
                    out.push_back(names.substr(start, comma - start));
                }
                if (comma == names.size()) {
                    break;
                }
                start = comma + 1;
            }
            return out;
        }

        std::string join_name_list(const std::vector<std::string> & names)
        {
            std::string out;
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                out += names[i];
            }
            return out;
        }

        bool parse_signature_blob(const std::vector<uint8_t> & signature_field,
                                  std::string & signature_algorithm,
                                  std::vector<uint8_t> & signature_blob)
        {
            size_t offset = 0;
            auto sig_alg = SshMessageCodec::read_string(signature_field.data(), signature_field.size(), offset);
            if (!sig_alg) {
                return false;
            }
            auto sig_data = SshMessageCodec::read_string(signature_field.data(), signature_field.size(), offset);
            if (!sig_data) {
                return false;
            }
            if (offset != signature_field.size()) {
                return false;
            }

            signature_algorithm = std::move(*sig_alg);
            signature_blob = to_bytes(*sig_data);
            return true;
        }

        bool signature_algorithm_matches_negotiated(std::string_view negotiated_host_key_name,
                                                    std::string_view signature_algorithm)
        {
            if (negotiated_host_key_name.empty()) {
                return true;
            }

            if (negotiated_host_key_name == signature_algorithm) {
                return true;
            }

            if (negotiated_host_key_name == "ssh-rsa") {
                return signature_algorithm == "ssh-rsa";
            }

            if ((negotiated_host_key_name == "rsa-sha2-256" || negotiated_host_key_name == "rsa-sha2-512") &&
                (signature_algorithm == "rsa-sha2-256" || signature_algorithm == "rsa-sha2-512")) {
                return true;
            }

            return false;
        }

        std::vector<uint8_t> build_rsa_public_key_der(const std::vector<uint8_t> & modulus,
                                                      const std::vector<uint8_t> & exponent)
        {
            OSSL_PARAM params[3];
            params[0] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_RSA_N,
                                                const_cast<uint8_t *>(modulus.data()),
                                                modulus.size());
            params[1] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_RSA_E,
                                                const_cast<uint8_t *>(exponent.data()),
                                                exponent.size());
            params[2] = OSSL_PARAM_construct_end();

            EVP_PKEY_CTX * ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
            EVP_PKEY * pkey = nullptr;
            std::vector<uint8_t> der;
            if (ctx && EVP_PKEY_fromdata_init(ctx) > 0 &&
                EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) > 0) {
                uint8_t * out_der = nullptr;
                const int out_len = i2d_PUBKEY(pkey, &out_der);
                if (out_len > 0 && out_der) {
                    der.assign(out_der, out_der + out_len);
                    OPENSSL_free(out_der);
                }
            }

            EVP_PKEY_free(pkey);
            EVP_PKEY_CTX_free(ctx);
            return der;
        }

        std::vector<uint8_t> build_ecdsa_public_key_der(const std::string & curve_name,
                                                        const std::vector<uint8_t> & public_point,
                                                        std::string & curve_out)
        {
            const char * group_name = nullptr;
            if (curve_name == "nistp256") {
                group_name = "prime256v1";
                curve_out = "P-256";
            } else if (curve_name == "nistp384") {
                group_name = "secp384r1";
                curve_out = "P-384";
            } else if (curve_name == "nistp521") {
                group_name = "secp521r1";
                curve_out = "P-521";
            } else {
                return {};
            }

            OSSL_PARAM params[3];
            params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                         const_cast<char *>(group_name),
                                                         0);
            params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
                                                          const_cast<uint8_t *>(public_point.data()),
                                                          public_point.size());
            params[2] = OSSL_PARAM_construct_end();

            EVP_PKEY_CTX * ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
            EVP_PKEY * pkey = nullptr;
            std::vector<uint8_t> der;
            if (ctx && EVP_PKEY_fromdata_init(ctx) > 0 &&
                EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) > 0) {
                uint8_t * out_der = nullptr;
                const int out_len = i2d_PUBKEY(pkey, &out_der);
                if (out_len > 0 && out_der) {
                    der.assign(out_der, out_der + out_len);
                    OPENSSL_free(out_der);
                }
            }

            EVP_PKEY_free(pkey);
            EVP_PKEY_CTX_free(ctx);
            return der;
        }

        bool verify_kex_reply_signature(SshCrypto * crypto,
                                        const std::vector<uint8_t> & exchange_hash,
                                        const std::vector<uint8_t> & host_key_blob,
                                        const std::vector<uint8_t> & signature_field,
                                        std::string_view negotiated_host_key_name)
        {
            if (!crypto) {
                return false;
            }

            std::string signature_algorithm;
            std::vector<uint8_t> signature_blob;
            if (!parse_signature_blob(signature_field, signature_algorithm, signature_blob)) {
                return false;
            }

            if (!signature_algorithm_matches_negotiated(negotiated_host_key_name, signature_algorithm)) {
                return false;
            }

            size_t key_offset = 0;
            auto key_type = SshMessageCodec::read_string(host_key_blob.data(), host_key_blob.size(), key_offset);
            if (!key_type) {
                return false;
            }

            if (signature_algorithm == "ssh-ed25519") {
                if (*key_type != "ssh-ed25519") {
                    return false;
                }
                auto raw_public_key = SshMessageCodec::read_string(host_key_blob.data(), host_key_blob.size(), key_offset);
                if (!raw_public_key || key_offset != host_key_blob.size()) {
                    return false;
                }
                auto key_bytes = to_bytes(*raw_public_key);
                return crypto->ed25519_verify(
                    key_bytes,
                    exchange_hash.data(), exchange_hash.size(),
                    signature_blob.data(), signature_blob.size());
            }

            if (signature_algorithm == "ssh-rsa" ||
                signature_algorithm == "rsa-sha2-256" ||
                signature_algorithm == "rsa-sha2-512") {
                if (*key_type != "ssh-rsa") {
                    return false;
                }
                auto exponent = SshMessageCodec::read_mpint(host_key_blob.data(), host_key_blob.size(), key_offset);
                auto modulus = SshMessageCodec::read_mpint(host_key_blob.data(), host_key_blob.size(), key_offset);
                if (!exponent || !modulus || key_offset != host_key_blob.size()) {
                    return false;
                }

                auto public_key_der = build_rsa_public_key_der(*modulus, *exponent);
                if (public_key_der.empty()) {
                    return false;
                }

                std::string hash_alg;
                if (signature_algorithm == "rsa-sha2-512") {
                    hash_alg = "sha512";
                } else if (signature_algorithm == "rsa-sha2-256") {
                    hash_alg = "sha256";
                } else {
                    hash_alg = "sha1";
                }

                return crypto->rsa_verify(
                    public_key_der,
                    hash_alg,
                    exchange_hash.data(), exchange_hash.size(),
                    signature_blob.data(), signature_blob.size());
            }

            if (signature_algorithm.rfind("ecdsa-sha2-", 0) == 0) {
                if (*key_type != signature_algorithm) {
                    return false;
                }
                auto curve_name = SshMessageCodec::read_string(host_key_blob.data(), host_key_blob.size(), key_offset);
                auto public_point = SshMessageCodec::read_string(host_key_blob.data(), host_key_blob.size(), key_offset);
                if (!curve_name || !public_point || key_offset != host_key_blob.size()) {
                    return false;
                }

                std::string curve;
                auto public_key_der = build_ecdsa_public_key_der(*curve_name, to_bytes(*public_point), curve);
                if (public_key_der.empty() || curve.empty()) {
                    return false;
                }

                return crypto->ecdsa_verify(
                    public_key_der,
                    curve,
                    exchange_hash.data(), exchange_hash.size(),
                    signature_blob.data(), signature_blob.size());
            }

            return false;
        }
    }

    SshTransport::SshTransport(SshAlgorithmRegistry * registry,
                               SshCrypto * crypto,
                               bool we_are_server)
        : registry_(registry), crypto_(crypto), we_are_server_(we_are_server)
    {
        state_ = SshTransportState::disconnected;
    }

    std::string SshTransport::build_version_string(const std::string & software_version) const
    {
        return SshVersionExchange::build_server_version(software_version);
    }

    std::optional<SshVersionInfo> SshTransport::process_client_version(const std::string & line)
    {
        auto info = SshVersionExchange::parse_version_line(line);
        if (!info)
            return std::nullopt;

        if (!SshVersionExchange::is_valid_protocol_version(info->protocol_version))
            return std::nullopt;

        client_version_ = info->raw_line;
        state_ = SshTransportState::version_exchanged;
        return info;
    }

    ByteBuffer SshTransport::build_kex_init(const SshServerConfig & config)
    {
        SshKexInitMessage msg;

        if (crypto_)
            crypto_->random_bytes(msg.cookie, 16);

        auto join_names = [](const std::vector<std::string> & names)->std::string
        {
            std::string result;
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0)
                    result += ',';
                result += names[i];
            }
            return result;
        };

        msg.kex_algorithms = join_names(config.kex_algorithms);
        msg.server_host_key_algorithms = join_names(config.host_key_algorithms);
        msg.encryption_algorithms_client_to_server = join_names(config.cipher_algorithms);
        msg.encryption_algorithms_server_to_client = join_names(config.cipher_algorithms);
        msg.mac_algorithms_client_to_server = join_names(config.mac_algorithms);
        msg.mac_algorithms_server_to_client = join_names(config.mac_algorithms);
        msg.compression_algorithms_client_to_server = join_names(config.compression_algorithms);
        msg.compression_algorithms_server_to_client = join_names(config.compression_algorithms);

        if (registry_) {
            auto kex_names = filter_supported_algorithms(config.kex_algorithms, registry_->supported_kex_names());
            if (!kex_names.empty()) {
                msg.kex_algorithms = join_names(kex_names);
            }

            auto hk_names = filter_supported_algorithms(config.host_key_algorithms, registry_->supported_host_key_names());
            if (!hk_names.empty()) {
                msg.server_host_key_algorithms = join_names(hk_names);
            }

            auto cipher_names = filter_supported_algorithms(config.cipher_algorithms, registry_->supported_cipher_names());
            if (!cipher_names.empty()) {
                msg.encryption_algorithms_client_to_server = join_names(cipher_names);
                msg.encryption_algorithms_server_to_client = join_names(cipher_names);
            }

            auto mac_names = filter_supported_algorithms(config.mac_algorithms, registry_->supported_mac_names());
            if (!mac_names.empty()) {
                msg.mac_algorithms_client_to_server = join_names(mac_names);
                msg.mac_algorithms_server_to_client = join_names(mac_names);
            }

            auto comp_names = filter_supported_algorithms(config.compression_algorithms, registry_->supported_compression_names());
            if (!comp_names.empty()) {
                msg.compression_algorithms_client_to_server = join_names(comp_names);
                msg.compression_algorithms_server_to_client = join_names(comp_names);
            }
        }

        msg.languages_client_to_server = "";
        msg.languages_server_to_client = "";
        msg.first_kex_packet_follows = false;
        msg.reserved = 0;

        auto encoded = SshMessageCodec::encode_kex_init(msg);

        our_kex_init_raw_.assign(
            reinterpret_cast<const uint8_t *>(encoded.read_ptr()),
            reinterpret_cast<const uint8_t *>(encoded.read_ptr()) + encoded.readable_bytes());

        state_ = SshTransportState::kex_init;
        return encoded;
    }

    std::optional<SshNegotiatedAlgorithms> SshTransport::process_kex_init(
        const SshKexInitMessage & msg,
        const SshServerConfig & config)
    {
        if (!registry_)
            return std::nullopt;

        const auto local_kex = filter_supported_algorithms(config.kex_algorithms, registry_->supported_kex_names());
        const auto local_host_key = filter_supported_algorithms(config.host_key_algorithms,
                                                                registry_->supported_host_key_names());
        const auto local_cipher = filter_supported_algorithms(config.cipher_algorithms,
                                                              registry_->supported_cipher_names());
        const auto local_mac = filter_supported_algorithms(config.mac_algorithms,
                                                           registry_->supported_mac_names());
        const auto local_compression = filter_supported_algorithms(config.compression_algorithms,
                                                                   registry_->supported_compression_names());

        std::vector<std::string> preferred_kex;
        std::vector<std::string> preferred_host_key;
        std::vector<std::string> preferred_cipher;
        std::vector<std::string> preferred_mac;
        std::vector<std::string> preferred_compression;

        std::string peer_kex_algorithms;
        std::string peer_host_key_algorithms;
        std::string peer_encryption_c2s;
        std::string peer_encryption_s2c;
        std::string peer_mac_c2s;
        std::string peer_mac_s2c;
        std::string peer_compression_c2s;
        std::string peer_compression_s2c;

        if (we_are_server_) {
            preferred_kex = split_name_list(msg.kex_algorithms);
            preferred_host_key = split_name_list(msg.server_host_key_algorithms);
            preferred_cipher = split_name_list(msg.encryption_algorithms_client_to_server);
            preferred_mac = split_name_list(msg.mac_algorithms_client_to_server);
            preferred_compression = split_name_list(msg.compression_algorithms_client_to_server);

            peer_kex_algorithms = join_name_list(local_kex);
            peer_host_key_algorithms = join_name_list(local_host_key);
            peer_encryption_c2s = join_name_list(local_cipher);
            peer_encryption_s2c = join_name_list(local_cipher);
            peer_mac_c2s = join_name_list(local_mac);
            peer_mac_s2c = join_name_list(local_mac);
            peer_compression_c2s = join_name_list(local_compression);
            peer_compression_s2c = join_name_list(local_compression);
        } else {
            preferred_kex = local_kex;
            preferred_host_key = local_host_key;
            preferred_cipher = local_cipher;
            preferred_mac = local_mac;
            preferred_compression = local_compression;

            peer_kex_algorithms = msg.kex_algorithms;
            peer_host_key_algorithms = msg.server_host_key_algorithms;
            peer_encryption_c2s = msg.encryption_algorithms_client_to_server;
            peer_encryption_s2c = msg.encryption_algorithms_server_to_client;
            peer_mac_c2s = msg.mac_algorithms_client_to_server;
            peer_mac_s2c = msg.mac_algorithms_server_to_client;
            peer_compression_c2s = msg.compression_algorithms_client_to_server;
            peer_compression_s2c = msg.compression_algorithms_server_to_client;
        }

        auto result = registry_->negotiate(
            preferred_kex,
            preferred_host_key,
            preferred_cipher,
            preferred_mac,
            preferred_compression,
            peer_kex_algorithms,
            peer_host_key_algorithms,
            peer_encryption_c2s,
            peer_encryption_s2c,
            peer_mac_c2s,
            peer_mac_s2c,
            peer_compression_c2s,
            peer_compression_s2c);

        if (result) {
            negotiated_ = *result;
            kex_algo_ = registry_->create_kex(result->kex_name);
            if (kex_algo_) {
                kex_algo_->set_crypto(crypto_);
            }
            ignore_next_kex_packet_ =
                msg.first_kex_packet_follows &&
                (first_name_in_list(msg.kex_algorithms) != result->kex_name ||
                 first_name_in_list(msg.server_host_key_algorithms) != result->host_key_name);
            state_ = SshTransportState::kex_init;
        }

        return result;
    }

    std::optional<std::vector<uint8_t> > SshTransport::generate_kex_public_key()
    {
        if (!kex_algo_) {
            return std::nullopt;
        }

        auto pub = kex_algo_->generate_public_key();
        if (pub.empty()) {
            return std::nullopt;
        }
        return pub;
    }

    bool SshTransport::start_kex(const std::vector<uint8_t> & peer_public)
    {
        if (!kex_algo_)
            return false;

        auto server_public = kex_algo_->generate_public_key();
        if (server_public.empty())
            return false;

        if (!kex_algo_->compute_shared_secret(peer_public, shared_secret_))
            return false;

        state_ = SshTransportState::kex_in_progress;
        return true;
    }

    std::optional<SshKexEcdhReplyMessage> SshTransport::process_kex_init_message(
        const std::vector<uint8_t> & client_public,
        const std::string & client_version,
        const std::string & server_version)
    {
        if (!kex_algo_ || !host_key_algo_)
            return std::nullopt;

        auto server_public = kex_algo_->generate_public_key();
        if (server_public.empty())
            return std::nullopt;

        if (!kex_algo_->compute_shared_secret(client_public, shared_secret_))
            return std::nullopt;

        const auto & client_kex_init = we_are_server_ ? peer_kex_init_raw_ : our_kex_init_raw_;
        const auto & server_kex_init = we_are_server_ ? our_kex_init_raw_ : peer_kex_init_raw_;

        exchange_hash_ = kex_algo_->compute_exchange_hash(
            client_version,
            server_version,
            client_kex_init,
            server_kex_init,
            host_key_algo_->public_key_blob(),
            client_public,
            server_public,
            shared_secret_);

        if (exchange_hash_.empty())
            return std::nullopt;

        if (session_id_.empty())
            session_id_ = SshKeyDerivation::derive_session_id(exchange_hash_);

        auto signature = host_key_algo_->sign(exchange_hash_);
        if (signature.empty())
            return std::nullopt;

        SshKexEcdhReplyMessage reply;
        reply.host_key_blob = host_key_algo_->public_key_blob();
        reply.server_public_key = std::move(server_public);
        reply.signature = std::move(signature);

        state_ = SshTransportState::kex_in_progress;
        return reply;
    }

    bool SshTransport::process_kex_reply_message(const SshKexEcdhReplyMessage & reply,
                                                 const std::string & client_version,
                                                 const std::string & server_version)
    {
        if (!kex_algo_) {
            return false;
        }

        const auto client_public = kex_algo_->public_key();
        if (client_public.empty()) {
            return false;
        }

        if (!kex_algo_->compute_shared_secret(reply.server_public_key, shared_secret_)) {
            return false;
        }

        const auto & client_kex_init = we_are_server_ ? peer_kex_init_raw_ : our_kex_init_raw_;
        const auto & server_kex_init = we_are_server_ ? our_kex_init_raw_ : peer_kex_init_raw_;

        exchange_hash_ = kex_algo_->compute_exchange_hash(
            client_version,
            server_version,
            client_kex_init,
            server_kex_init,
            reply.host_key_blob,
            client_public,
            reply.server_public_key,
            shared_secret_);

        if (exchange_hash_.empty()) {
            return false;
        }

        if (session_id_.empty()) {
            session_id_ = SshKeyDerivation::derive_session_id(exchange_hash_);
        }

        if (!verify_kex_reply_signature(
                crypto_,
                exchange_hash_,
                reply.host_key_blob,
                reply.signature,
                negotiated_.host_key_name)) {
            return false;
        }

        state_ = SshTransportState::kex_in_progress;
        return true;
    }

    bool SshTransport::process_newkeys()
    {
        cipher_ctx_.activate(negotiated_, shared_secret_, exchange_hash_,
                             session_id_, we_are_server_, registry_);

        state_ = SshTransportState::newkeys;
        return true;
    }

    ByteBuffer SshTransport::encode_packet(const uint8_t * payload, size_t len)
    {
        return SshPacketCodec::encode(send_seq_, payload, len, &cipher_ctx_);
    }

    std::optional<std::vector<uint8_t> > SshTransport::decode_packet(const uint8_t * data, size_t len)
    {
        return SshPacketCodec::decode(recv_seq_, data, len, &cipher_ctx_);
    }

    SshPacketCodec::ParseResult SshTransport::try_parse_packet(const ByteBuffer & buf)
    {
        return SshPacketCodec::try_parse(buf, is_encrypted(), &cipher_ctx_, recv_seq_);
    }

    bool SshTransport::consume_pending_kex_guess()
    {
        if (!ignore_next_kex_packet_) {
            return false;
        }

        ignore_next_kex_packet_ = false;
        return true;
    }

    void SshTransport::reset_for_rekey()
    {
        kex_algo_.reset();
        shared_secret_.clear();
        exchange_hash_.clear();
        our_kex_init_raw_.clear();
        peer_kex_init_raw_.clear();
        ignore_next_kex_packet_ = false;
        state_ = SshTransportState::kex_init;
    }
}
