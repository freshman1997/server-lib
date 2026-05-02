#ifndef __NET_SSH_TRANSPORT_SSH_TRANSPORT_H__
#define __NET_SSH_TRANSPORT_SSH_TRANSPORT_H__

#include "algorithm/ssh_kex_algorithm.h"
#include "algorithm/ssh_host_key_algorithm.h"
#include "algorithm/ssh_algorithm_registry.h"
#include "crypto/ssh_crypto.h"
#include "ssh_config.h"
#include "protocol/ssh_constants.h"
#include "protocol/ssh_structures.h"
#include "transport/ssh_cipher_context.h"
#include "transport/ssh_packet_codec.h"
#include "transport/ssh_version_exchange.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    using ::yuan::buffer::ByteBuffer;

    enum class SshTransportState {
        disconnected,
        version_exchange,
        version_exchanged,
        kex_init,
        kex_in_progress,
        newkeys,
        active
    };

    class SshTransport
    {
    public:
        SshTransport(SshAlgorithmRegistry *registry,
                     SshCrypto *crypto,
                     bool we_are_server);

        void set_we_are_server(bool v)
        {
            we_are_server_ = v;
        }

        std::string build_version_string(const std::string &software_version) const;

        std::optional<SshVersionInfo> process_client_version(const std::string &line);

        ByteBuffer build_kex_init(const SshServerConfig &config);

        std::optional<SshNegotiatedAlgorithms> process_kex_init(const SshKexInitMessage &msg,
                                                                const SshServerConfig &config);

        bool start_kex(const std::vector<uint8_t> &peer_public);

        std::optional<std::vector<uint8_t> > generate_kex_public_key();

        std::optional<SshKexEcdhReplyMessage> process_kex_init_message(
            const std::vector<uint8_t> &client_public,
            const std::string &client_version,
            const std::string &server_version);

        bool process_kex_reply_message(const SshKexEcdhReplyMessage &reply,
                                       const std::string &client_version,
                                       const std::string &server_version);

        bool process_newkeys();

        ByteBuffer encode_packet(const uint8_t *payload, size_t len);

        std::optional<std::vector<uint8_t> > decode_packet(const uint8_t *data, size_t len);

        SshPacketCodec::ParseResult try_parse_packet(const ByteBuffer &buf);

        void increment_send_seq()
        {
            ++send_seq_;
        }
        void increment_recv_seq()
        {
            ++recv_seq_;
        }

        uint32_t send_seq() const
        {
            return send_seq_;
        }
        uint32_t recv_seq() const
        {
            return recv_seq_;
        }

        SshTransportState state() const
        {
            return state_;
        }
        bool is_encrypted() const
        {
            return cipher_ctx_.is_active();
        }

        SshCipherContext &cipher_context()
        {
            return cipher_ctx_;
        }
        const SshCipherContext &cipher_context() const
        {
            return cipher_ctx_;
        }

        const std::vector<uint8_t> &session_id() const
        {
            return session_id_;
        }
        const std::string &client_version() const
        {
            return client_version_;
        }
        const std::string &server_version() const
        {
            return server_version_;
        }

        void set_host_key_algorithm(std::unique_ptr<SshHostKeyAlgorithm> algo)
        {
            owned_host_key_algo_ = std::move(algo);
            host_key_algo_ = owned_host_key_algo_ ? &*owned_host_key_algo_ : nullptr;
        }

        void set_host_key_algorithm(SshHostKeyAlgorithm * algo)
        {
            owned_host_key_algo_.reset();
            host_key_algo_ = algo;
        }
        void set_client_version(const std::string &v)
        {
            client_version_ = v;
        }
        void set_server_version(const std::string &v)
        {
            server_version_ = v;
        }

        const std::vector<uint8_t> &our_kex_init_raw() const
        {
            return our_kex_init_raw_;
        }
        void set_our_kex_init_raw(std::vector<uint8_t> raw)
        {
            our_kex_init_raw_ = std::move(raw);
        }
        void set_peer_kex_init_raw(std::vector<uint8_t> raw)
        {
            peer_kex_init_raw_ = std::move(raw);
        }

        bool consume_pending_kex_guess();

        void reset_for_rekey();

    private:
        SshTransportState state_ = SshTransportState::disconnected;
        SshCipherContext cipher_ctx_;
        SshAlgorithmRegistry *registry_ = nullptr;
        SshCrypto *crypto_ = nullptr;

        std::unique_ptr<SshKexAlgorithm> kex_algo_;
        std::unique_ptr<SshHostKeyAlgorithm> owned_host_key_algo_;
        SshHostKeyAlgorithm *host_key_algo_ = nullptr;

        std::vector<uint8_t> session_id_;
        std::vector<uint8_t> exchange_hash_;
        std::vector<uint8_t> shared_secret_;

        uint32_t send_seq_ = 0;
        uint32_t recv_seq_ = 0;

        std::string client_version_;
        std::string server_version_;

        std::vector<uint8_t> our_kex_init_raw_;
        std::vector<uint8_t> peer_kex_init_raw_;

        SshNegotiatedAlgorithms negotiated_;
        bool ignore_next_kex_packet_ = false;

        bool we_are_server_ = true;
    };
}

#endif
