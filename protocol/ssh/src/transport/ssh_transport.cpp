#include "transport/ssh_transport.h"
#include "protocol/ssh_message_codec.h"
#include "crypto/ssh_key_derivation.h"
#include "ssh_config.h"
#include <algorithm>
#include <cstring>

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

        auto result = registry_->negotiate(
            filter_supported_algorithms(config.kex_algorithms, registry_->supported_kex_names()),
            filter_supported_algorithms(config.host_key_algorithms, registry_->supported_host_key_names()),
            filter_supported_algorithms(config.cipher_algorithms, registry_->supported_cipher_names()),
            filter_supported_algorithms(config.mac_algorithms, registry_->supported_mac_names()),
            filter_supported_algorithms(config.compression_algorithms, registry_->supported_compression_names()),
            msg.kex_algorithms,
            msg.server_host_key_algorithms,
            msg.encryption_algorithms_client_to_server,
            msg.encryption_algorithms_server_to_client,
            msg.mac_algorithms_client_to_server,
            msg.mac_algorithms_server_to_client,
            msg.compression_algorithms_client_to_server,
            msg.compression_algorithms_server_to_client);

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
