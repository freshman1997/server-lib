#include "transport/ssh_cipher_context.h"
#include "crypto/ssh_key_derivation.h"
#include "protocol/ssh_constants.h"
#include <cstring>

namespace yuan::net::ssh
{
    namespace
    {
        constexpr size_t kMaxPaddingLength = 255;
    }

    SshCipher *SshCipherContext::outbound_cipher() const
    {
        return we_are_server_ ? (server_cipher_ ? &*server_cipher_ : nullptr)
                             : (client_cipher_ ? &*client_cipher_ : nullptr);
    }

    SshCipher *SshCipherContext::inbound_cipher() const
    {
        return we_are_server_ ? (client_cipher_ ? &*client_cipher_ : nullptr)
                             : (server_cipher_ ? &*server_cipher_ : nullptr);
    }

    SshMac *SshCipherContext::outbound_mac() const
    {
        return we_are_server_ ? (server_mac_ ? &*server_mac_ : nullptr)
                             : (client_mac_ ? &*client_mac_ : nullptr);
    }

    SshMac *SshCipherContext::inbound_mac() const
    {
        return we_are_server_ ? (client_mac_ ? &*client_mac_ : nullptr)
                             : (server_mac_ ? &*server_mac_ : nullptr);
    }

    bool SshCipherContext::is_chacha20_poly1305() const
    {
        return is_chacha20_;
    }

    bool SshCipherContext::try_decrypt_packet_length(uint32_t seq,
                                                     const uint8_t * data, size_t len,
                                                     uint32_t & out_packet_length) const
    {
        if (!active_)
            return false;

        uint8_t seq_bytes[4];
        seq_bytes[0] = static_cast<uint8_t>((seq >> 24) & 0xFF);
        seq_bytes[1] = static_cast<uint8_t>((seq >> 16) & 0xFF);
        seq_bytes[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
        seq_bytes[3] = static_cast<uint8_t>(seq & 0xFF);

        if (is_chacha20_) {
            if (len < 4)
                return false;

            SshCipher *cipher = we_are_server_ ? (client_cipher_ ? &*client_cipher_ : nullptr)
                                               : (server_cipher_ ? &*server_cipher_ : nullptr);
            if (!cipher)
                return false;

            uint8_t dec_length[4];
            if (!cipher->decrypt_length(data, 4, seq_bytes, dec_length))
                return false;

            out_packet_length = (static_cast<uint32_t>(dec_length[0]) << 24) |
                                (static_cast<uint32_t>(dec_length[1]) << 16) |
                                (static_cast<uint32_t>(dec_length[2]) << 8) |
                                static_cast<uint32_t>(dec_length[3]);
            return true;
        }

        SshCipher *cipher = inbound_cipher();
        if (!cipher || len < 4)
            return false;

        uint8_t dec_length[4];
        if (!cipher->decrypt_length(data, len, seq_bytes, dec_length))
            return false;

        out_packet_length = (static_cast<uint32_t>(dec_length[0]) << 24) |
                            (static_cast<uint32_t>(dec_length[1]) << 16) |
                            (static_cast<uint32_t>(dec_length[2]) << 8) |
                            static_cast<uint32_t>(dec_length[3]);
        return true;
    }

    void SshCipherContext::activate(const SshNegotiatedAlgorithms & negotiated,
                                    const std::vector<uint8_t> & K,
                                    const std::vector<uint8_t> & H,
                                    const std::vector<uint8_t> & session_id,
                                    bool we_are_server,
                                    SshAlgorithmRegistry * registry)
    {
        negotiated_ = negotiated;
        we_are_server_ = we_are_server;
        is_chacha20_ = (negotiated.client_to_server_cipher_name == "chacha20-poly1305@openssh.com" ||
                        negotiated.server_to_client_cipher_name == "chacha20-poly1305@openssh.com");

        if (!registry)
            return;

        client_cipher_ = registry->create_cipher(negotiated.client_to_server_cipher_name);
        server_cipher_ = registry->create_cipher(negotiated.server_to_client_cipher_name);

        if (!client_cipher_ || !server_cipher_)
            return;

        size_t c2s_key_len = client_cipher_->key_size();
        size_t s2c_key_len = server_cipher_->key_size();
        size_t c2s_iv_len = client_cipher_->iv_size();
        size_t s2c_iv_len = server_cipher_->iv_size();

        std::string hash_name = negotiated.kex_hash_name;
        if (hash_name.empty())
            hash_name = "sha256";

        auto iv_c2s = SshKeyDerivation::derive_key(K, H, session_id, 'A', c2s_iv_len, hash_name);
        auto iv_s2c = SshKeyDerivation::derive_key(K, H, session_id, 'B', s2c_iv_len, hash_name);
        auto key_c2s = SshKeyDerivation::derive_key(K, H, session_id, 'C', c2s_key_len, hash_name);
        auto key_s2c = SshKeyDerivation::derive_key(K, H, session_id, 'D', s2c_key_len, hash_name);

        client_cipher_->init(key_c2s, iv_c2s);
        server_cipher_->init(key_s2c, iv_s2c);

        bool is_aead_cipher = client_cipher_->is_aead();

        if (!is_aead_cipher) {
            client_mac_ = registry->create_mac(negotiated.client_to_server_mac_name);
            server_mac_ = registry->create_mac(negotiated.server_to_client_mac_name);

            if (client_mac_ && server_mac_) {
                size_t c2s_mac_key_len = client_mac_->key_size();
                size_t s2c_mac_key_len = server_mac_->key_size();

                auto mac_key_c2s = SshKeyDerivation::derive_key(K, H, session_id, 'E', c2s_mac_key_len, hash_name);
                auto mac_key_s2c = SshKeyDerivation::derive_key(K, H, session_id, 'F', s2c_mac_key_len, hash_name);

                client_mac_->init(mac_key_c2s);
                server_mac_->init(mac_key_s2c);
            }
        }

        auto create_compressor = [&](const std::string & name)->std::unique_ptr<SshCompression>
        {
            auto comp = registry->create_compression(name);
            if (comp && !comp->init())
                return nullptr;
            return comp;
        };

        client_compressor_ = create_compressor(negotiated.client_to_server_compression_name);
        server_compressor_ = create_compressor(negotiated.server_to_client_compression_name);

        active_ = true;
    }

    std::vector<uint8_t> SshCipherContext::encrypt_packet(uint32_t seq,
                                                          const uint8_t * data, size_t len,
                                                          uint8_t padding_len)
    {
        (void)padding_len;
        if (!active_)
            return std::vector<uint8_t>(data, data + len);

        SshCipher *cipher = outbound_cipher();
        if (!cipher)
            return std::vector<uint8_t>(data, data + len);

        uint8_t seq_bytes[4];
        seq_bytes[0] = static_cast<uint8_t>((seq >> 24) & 0xFF);
        seq_bytes[1] = static_cast<uint8_t>((seq >> 16) & 0xFF);
        seq_bytes[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
        seq_bytes[3] = static_cast<uint8_t>(seq & 0xFF);

        uint32_t pkt_len = static_cast<uint32_t>(len);
        uint8_t pkt_len_bytes[4];
        pkt_len_bytes[0] = static_cast<uint8_t>((pkt_len >> 24) & 0xFF);
        pkt_len_bytes[1] = static_cast<uint8_t>((pkt_len >> 16) & 0xFF);
        pkt_len_bytes[2] = static_cast<uint8_t>((pkt_len >> 8) & 0xFF);
        pkt_len_bytes[3] = static_cast<uint8_t>(pkt_len & 0xFF);

        if (cipher->is_aead()) {
            auto encrypted = cipher->encrypt_aead(
                pkt_len_bytes, 4,
                data, len,
                seq_bytes);

            if (is_chacha20_) {
                return encrypted;
            }
            return encrypted;
        }

        auto encrypted = cipher->encrypt(data, len);

        SshMac *mac = outbound_mac();
        if (mac) {
            auto mac_val = mac->compute(seq, data, len);
            encrypted.insert(encrypted.end(), mac_val.begin(), mac_val.end());
        }

        return encrypted;
    }

    bool SshCipherContext::decrypt_packet(uint32_t seq,
                                          const uint8_t * data, size_t len,
                                          std::vector<uint8_t> & out_payload)
    {
        if (!active_) {
            out_payload.assign(data, data + len);
            return true;
        }

        SshCipher *cipher = inbound_cipher();
        if (!cipher) {
            out_payload.assign(data, data + len);
            return true;
        }

        uint8_t seq_bytes[4];
        seq_bytes[0] = static_cast<uint8_t>((seq >> 24) & 0xFF);
        seq_bytes[1] = static_cast<uint8_t>((seq >> 16) & 0xFF);
        seq_bytes[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
        seq_bytes[3] = static_cast<uint8_t>(seq & 0xFF);

        if (cipher->is_aead()) {
            if (is_chacha20_) {
                if (len < 4 + cipher->tag_size())
                    return false;

                size_t ciphertext_len = len - 4;
                const uint8_t *encrypted_length = data;
                const uint8_t *ciphertext_and_tag = data + 4;

                size_t tag_len = cipher->tag_size();
                if (ciphertext_len < tag_len)
                    return false;

                const uint8_t *ciphertext = ciphertext_and_tag;
                const uint8_t *tag = ciphertext_and_tag + (ciphertext_len - tag_len);
                size_t actual_ct_len = ciphertext_len - tag_len;

                return cipher->decrypt_aead(
                    encrypted_length, 4,
                    ciphertext, actual_ct_len,
                    tag, tag_len,
                    seq_bytes,
                    out_payload);
            }

            size_t tag_len = cipher->tag_size();
            if (len < tag_len)
                return false;

            size_t ciphertext_len = len - tag_len;
            const uint8_t *ciphertext = data;
            const uint8_t *tag = data + ciphertext_len;

            uint8_t aad[4];
            uint32_t pkt_len = static_cast<uint32_t>(ciphertext_len);
            aad[0] = static_cast<uint8_t>((pkt_len >> 24) & 0xFF);
            aad[1] = static_cast<uint8_t>((pkt_len >> 16) & 0xFF);
            aad[2] = static_cast<uint8_t>((pkt_len >> 8) & 0xFF);
            aad[3] = static_cast<uint8_t>(pkt_len & 0xFF);

            return cipher->decrypt_aead(aad, 4,
                                        ciphertext, ciphertext_len,
                                        tag, tag_len,
                                        seq_bytes,
                                        out_payload);
        }

        SshMac *mac = inbound_mac();
        size_t mac_len = 0;
        if (mac)
            mac_len = mac->digest_size();

        if (len < mac_len)
            return false;

        size_t encrypted_len = len - mac_len;
        const uint8_t *encrypted_data = data;
        const uint8_t *mac_data_ptr = data + encrypted_len;

        auto decrypted = cipher->decrypt(encrypted_data, encrypted_len);
        if (decrypted.empty())
            return false;

        if (mac && !mac->verify(seq, decrypted.data(), decrypted.size(),
                                mac_data_ptr, mac_len))
            return false;

        out_payload = std::move(decrypted);
        return true;
    }

    size_t SshCipherContext::encrypt_overhead(size_t payload_len) const
    {
        if (!active_)
            return 0;

        size_t overhead = 5;
        SshCipher *cipher = outbound_cipher();
        if (cipher) {
            if (cipher->is_aead())
                overhead += cipher->tag_size();
        }

        SshMac *mac = outbound_mac();
        if (mac && (!cipher || !cipher->is_aead()))
            overhead += mac->digest_size();

        return overhead;
    }

    size_t SshCipherContext::decryption_overhead() const
    {
        if (!active_)
            return 0;

        size_t overhead = 0;

        SshCipher *cipher = inbound_cipher();
        if (cipher) {
            if (cipher->is_aead())
                overhead += cipher->tag_size();
        }

        SshMac *mac = inbound_mac();
        if (mac && (!cipher || !cipher->is_aead()))
            overhead += mac->digest_size();

        return overhead;
    }

    bool SshCipherContext::is_aead() const
    {
        if (!active_)
            return false;
        SshCipher *cipher = outbound_cipher();
        return cipher && cipher->is_aead();
    }

    size_t SshCipherContext::block_size() const
    {
        if (!active_)
            return 8;
        SshCipher *cipher = outbound_cipher();
        return cipher ? cipher->block_size() : 8;
    }

    size_t SshCipherContext::mac_size() const
    {
        if (!active_)
            return 0;
        SshMac *mac = outbound_mac();
        return mac ? mac->digest_size() : 0;
    }

    size_t SshCipherContext::tag_size() const
    {
        if (!active_)
            return 0;
        SshCipher *cipher = outbound_cipher();
        return cipher ? cipher->tag_size() : 0;
    }

    uint8_t SshCipherContext::calculate_padding_length(size_t payload_len) const
    {
        size_t bs = block_size();
        size_t min_padding = SSH_PACKET_MIN_PADDING;

        size_t unpadded = 1 + payload_len;
        size_t pad_len = bs - (unpadded % bs);
        if (pad_len < min_padding)
            pad_len += bs;

        if (pad_len > kMaxPaddingLength)
            pad_len = kMaxPaddingLength;

        return static_cast<uint8_t>(pad_len);
    }
}
