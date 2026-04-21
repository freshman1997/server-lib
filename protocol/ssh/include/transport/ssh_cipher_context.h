#ifndef __NET_SSH_TRANSPORT_SSH_CIPHER_CONTEXT_H__
#define __NET_SSH_TRANSPORT_SSH_CIPHER_CONTEXT_H__

#include "algorithm/ssh_cipher.h"
#include "algorithm/ssh_mac.h"
#include "algorithm/ssh_compression.h"
#include "algorithm/ssh_algorithm_registry.h"
#include "protocol/ssh_structures.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace yuan::net::ssh
{
    class SshCipherContext
    {
    public:
        SshCipherContext() = default;

        void activate(const SshNegotiatedAlgorithms &negotiated,
                      const std::vector<uint8_t> &K,
                      const std::vector<uint8_t> &H,
                      const std::vector<uint8_t> &session_id,
                      bool we_are_server,
                      SshAlgorithmRegistry *registry);

        bool is_chacha20_poly1305() const;

        bool try_decrypt_packet_length(uint32_t seq,
                                       const uint8_t *data, size_t len,
                                       uint32_t &out_packet_length) const;

        std::vector<uint8_t> encrypt_packet(uint32_t seq,
                                            const uint8_t *data, size_t len,
                                            uint8_t padding_len);

        bool decrypt_packet(uint32_t seq,
                            const uint8_t *data, size_t len,
                            std::vector<uint8_t> &out_payload);

        size_t encrypt_overhead(size_t payload_len) const;

        size_t decryption_overhead() const;

        bool is_active() const
        {
            return active_;
        }

        bool is_aead() const;

        size_t block_size() const;

        size_t mac_size() const;

        size_t tag_size() const;

        SshCipher *server_cipher() const
        {
            return server_cipher_ ? &*server_cipher_ : nullptr;
        }
        SshCipher *client_cipher() const
        {
            return client_cipher_ ? &*client_cipher_ : nullptr;
        }
        SshMac *server_mac() const
        {
            return server_mac_ ? &*server_mac_ : nullptr;
        }
        SshMac *client_mac() const
        {
            return client_mac_ ? &*client_mac_ : nullptr;
        }
        SshCompression *server_compressor() const
        {
            return server_compressor_ ? &*server_compressor_ : nullptr;
        }
        SshCompression *client_compressor() const
        {
            return client_compressor_ ? &*client_compressor_ : nullptr;
        }

        uint8_t calculate_padding_length(size_t payload_len) const;

    private:
        SshCipher *outbound_cipher() const;
        SshCipher *inbound_cipher() const;
        SshMac *outbound_mac() const;
        SshMac *inbound_mac() const;

        bool active_ = false;
        bool we_are_server_ = true;
        bool is_chacha20_ = false;

        std::unique_ptr<SshCipher> client_cipher_;
        std::unique_ptr<SshCipher> server_cipher_;
        std::unique_ptr<SshMac> client_mac_;
        std::unique_ptr<SshMac> server_mac_;
        std::unique_ptr<SshCompression> client_compressor_;
        std::unique_ptr<SshCompression> server_compressor_;

        SshNegotiatedAlgorithms negotiated_;
    };
}

#endif
