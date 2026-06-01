#include "transport/ssh_packet_codec.h"
#include "protocol/ssh_constants.h"
#include <cstring>

namespace yuan::net::ssh
{
    size_t SshPacketCodec::calculate_padding(size_t payload_len, size_t block_size)
    {
        // RFC 4253: packet_length field is NOT encrypted-block aligned part,
        // alignment applies to: padding_length(1) + payload + random_padding.
        // Since final on-wire encrypted bytes include packet_length(4),
        // choose padding so that (4 + 1 + payload + padding) is block-aligned.
        size_t unpadded = 4 + 1 + payload_len;
        size_t pad = block_size - (unpadded % block_size);
        if (pad < kMinPadding)
            pad += block_size;
        if (pad > kMaxPadding)
            return kMinPadding;
        return pad;
    }

    SshPacketCodec::ParseResult SshPacketCodec::try_parse(const ByteBuffer & buf,
                                                          bool encrypted,
                                                          SshCipherContext * cipher_ctx,
                                                          uint32_t seq)
    {
        ParseResult result;

        if (!encrypted || !cipher_ctx || !cipher_ctx->is_active()) {
            if (buf.readable_bytes() < 4) {
                result.complete = false;
                return result;
            }

            const uint8_t *ptr = reinterpret_cast<const uint8_t *>(buf.read_ptr());
            uint32_t packet_length = (static_cast<uint32_t>(ptr[0]) << 24) |
                                     (static_cast<uint32_t>(ptr[1]) << 16) |
                                     (static_cast<uint32_t>(ptr[2]) << 8) |
                                     static_cast<uint32_t>(ptr[3]);

            if (packet_length > SSH_MAX_PACKET_SIZE) {
                result.complete = false;
                result.invalid = true;
                return result;
            }

            size_t total = 4 + packet_length;
            if (buf.readable_bytes() < total) {
                result.complete = false;
                return result;
            }

            result.complete = true;
            result.total_bytes = total;
            return result;
        }

        if (cipher_ctx->is_chacha20_poly1305()) {
            size_t tag_len = cipher_ctx->tag_size();

            if (buf.readable_bytes() < 4 + tag_len) {
                result.complete = false;
                return result;
            }

            const uint8_t *ptr = reinterpret_cast<const uint8_t *>(buf.read_ptr());

            uint32_t packet_length = 0;
            if (!cipher_ctx->try_decrypt_packet_length(seq, ptr, buf.readable_bytes(), packet_length)) {
                result.complete = false;
                return result;
            }

            if (packet_length > SSH_MAX_PACKET_SIZE) {
                result.complete = false;
                result.invalid = true;
                return result;
            }

            size_t total = 4 + packet_length + tag_len;
            if (buf.readable_bytes() < total) {
                result.complete = false;
                return result;
            }

            result.complete = true;
            result.total_bytes = total;
            return result;
        }

        if (cipher_ctx->is_aead()) {
            if (buf.readable_bytes() < 4) {
                result.complete = false;
                return result;
            }

            const uint8_t *ptr = reinterpret_cast<const uint8_t *>(buf.read_ptr());
            uint32_t packet_length = (static_cast<uint32_t>(ptr[0]) << 24) |
                                     (static_cast<uint32_t>(ptr[1]) << 16) |
                                     (static_cast<uint32_t>(ptr[2]) << 8) |
                                     static_cast<uint32_t>(ptr[3]);

            if (packet_length > SSH_MAX_PACKET_SIZE) {
                result.complete = false;
                result.invalid = true;
                return result;
            }

            size_t total = 4 + packet_length + cipher_ctx->tag_size();
            if (buf.readable_bytes() < total) {
                result.complete = false;
                return result;
            }

            result.complete = true;
            result.total_bytes = total;
            return result;
        }

        size_t preview_len = cipher_ctx->block_size();
        if (preview_len < 4) {
            preview_len = 4;
        }

        if (buf.readable_bytes() < preview_len) {
            result.complete = false;
            return result;
        }

        const uint8_t *ptr = reinterpret_cast<const uint8_t *>(buf.read_ptr());
        uint32_t packet_length = 0;
        if (!cipher_ctx->try_decrypt_packet_length(seq, ptr, preview_len, packet_length)) {
            result.complete = false;
            return result;
        }

        if (packet_length > SSH_MAX_PACKET_SIZE) {
            result.complete = false;
            result.invalid = true;
            return result;
        }

        size_t total = 4 + packet_length + cipher_ctx->mac_size();
        if (buf.readable_bytes() < total) {
            result.complete = false;
            return result;
        }

        result.complete = true;
        result.total_bytes = total;
        return result;
    }

    ByteBuffer SshPacketCodec::encode(uint32_t seq,
                                      const uint8_t * payload, size_t len,
                                      SshCipherContext * cipher_ctx)
    {
        size_t block_size = 8;
        if (cipher_ctx && cipher_ctx->is_active())
            block_size = cipher_ctx->block_size();

        size_t padding_len = calculate_padding(len, block_size);
        size_t packet_length = 1 + len + padding_len;

        ByteBuffer plaintext;
        plaintext.ensure_writable(4 + packet_length);
        plaintext.append_u32(static_cast<uint32_t>(packet_length));
        plaintext.append_u8(static_cast<uint8_t>(padding_len));
        plaintext.append(payload, len);

        std::vector<uint8_t> padding(padding_len);
        if (cipher_ctx && cipher_ctx->is_active()) {
        } else {
            for (size_t i = 0; i < padding_len; ++i)
                padding[i] = static_cast<uint8_t>(i);
        }
        plaintext.append(padding.data(), padding_len);

        if (!cipher_ctx || !cipher_ctx->is_active()) {
            return plaintext;
        }

        const uint8_t *plain_ptr = reinterpret_cast<const uint8_t *>(plaintext.read_ptr());
        auto encrypted = cipher_ctx->encrypt_packet(
            seq,
            cipher_ctx->is_aead() ? (plain_ptr + 4) : plain_ptr,
            cipher_ctx->is_aead() ? packet_length : (4 + packet_length),
            static_cast<uint8_t>(padding_len));

        ByteBuffer final_out;
        final_out.ensure_writable(cipher_ctx->is_aead() && !cipher_ctx->is_chacha20_poly1305()
                                      ? (4 + encrypted.size())
                                      : encrypted.size());

        if (cipher_ctx->is_chacha20_poly1305()) {
            final_out.append(encrypted.data(), encrypted.size());
        } else if (cipher_ctx->is_aead()) {
            final_out.append(plain_ptr, 4);
            final_out.append(encrypted.data(), encrypted.size());
        } else {
            final_out.append(encrypted.data(), encrypted.size());
        }

        return final_out;
    }

    std::optional<std::vector<uint8_t> > SshPacketCodec::decode(uint32_t seq,
                                                                const uint8_t * data, size_t len,
                                                                SshCipherContext * cipher_ctx)
    {
        if (len < 4)
            return std::nullopt;

        if (!cipher_ctx || !cipher_ctx->is_active()) {
            uint32_t packet_length = (static_cast<uint32_t>(data[0]) << 24) |
                                     (static_cast<uint32_t>(data[1]) << 16) |
                                     (static_cast<uint32_t>(data[2]) << 8) |
                                     static_cast<uint32_t>(data[3]);

            if (packet_length > SSH_MAX_PACKET_SIZE)
                return std::nullopt;

            size_t rest = packet_length;
            if (len < 4 + rest)
                return std::nullopt;

            if (rest < 1)
                return std::nullopt;

            uint8_t padding_len = data[4];
            if (rest < 1u + padding_len)
                return std::nullopt;

            size_t payload_len = rest - 1 - padding_len;
            const uint8_t *payload_start = data + 5;

            std::vector<uint8_t> payload(payload_start, payload_start + payload_len);
            return payload;
        }

        if (cipher_ctx->is_chacha20_poly1305()) {
            std::vector<uint8_t> decrypted;
            if (!cipher_ctx->decrypt_packet(seq, data, len, decrypted))
                return std::nullopt;

            if (decrypted.empty())
                return std::nullopt;

            uint8_t padding_len = decrypted[0];
            if (decrypted.size() < 1u + padding_len)
                return std::nullopt;

            size_t payload_len = decrypted.size() - 1 - padding_len;
            std::vector<uint8_t> payload(decrypted.begin() + 1, decrypted.begin() + 1 + payload_len);
            return payload;
        }

        if (cipher_ctx->is_aead()) {
            uint32_t packet_length = (static_cast<uint32_t>(data[0]) << 24) |
                                     (static_cast<uint32_t>(data[1]) << 16) |
                                     (static_cast<uint32_t>(data[2]) << 8) |
                                     static_cast<uint32_t>(data[3]);

            if (packet_length > SSH_MAX_PACKET_SIZE)
                return std::nullopt;

            size_t encrypted_len = len - 4;
            std::vector<uint8_t> decrypted;
            if (!cipher_ctx->decrypt_packet(seq, data + 4, encrypted_len, decrypted))
                return std::nullopt;

            if (decrypted.empty())
                return std::nullopt;

            uint8_t padding_len = decrypted[0];
            if (decrypted.size() < 1u + padding_len)
                return std::nullopt;

            size_t payload_len = decrypted.size() - 1 - padding_len;
            std::vector<uint8_t> payload(decrypted.begin() + 1, decrypted.begin() + 1 + payload_len);
            return payload;
        }

        size_t mac_len = cipher_ctx->mac_size();
        if (len < 4 + mac_len)
            return std::nullopt;

        std::vector<uint8_t> decrypted;
        if (!cipher_ctx->decrypt_packet(seq, data, len, decrypted))
            return std::nullopt;

        if (decrypted.size() < 5)
            return std::nullopt;

        uint32_t packet_length = (static_cast<uint32_t>(decrypted[0]) << 24) |
                                 (static_cast<uint32_t>(decrypted[1]) << 16) |
                                 (static_cast<uint32_t>(decrypted[2]) << 8) |
                                 static_cast<uint32_t>(decrypted[3]);

        if (packet_length > SSH_MAX_PACKET_SIZE)
            return std::nullopt;

        if (decrypted.size() != 4u + packet_length)
            return std::nullopt;

        uint8_t padding_len = decrypted[4];
        if (packet_length < 1u + padding_len)
            return std::nullopt;

        size_t payload_len = packet_length - 1 - padding_len;
        std::vector<uint8_t> payload(decrypted.begin() + 5, decrypted.begin() + 5 + payload_len);
        return payload;
    }
}
