#ifndef __NET_SSH_PROTOCOL_SSH_MESSAGE_CODEC_H__
#define __NET_SSH_PROTOCOL_SSH_MESSAGE_CODEC_H__

#include "protocol/ssh_constants.h"
#include "protocol/ssh_structures.h"
#include "buffer/byte_buffer.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshMessageCodec
    {
    public:
        static void write_boolean(ByteBuffer &buf, bool value);
        static void write_uint32(ByteBuffer &buf, uint32_t value);
        static void write_uint64(ByteBuffer &buf, uint64_t value);
        static void write_string(ByteBuffer &buf, const std::string &value);
        static void write_mpint(ByteBuffer &buf, const std::vector<uint8_t> &value);
        static void write_name_list(ByteBuffer &buf, const std::string &value);
        static void write_raw(ByteBuffer &buf, const uint8_t *data, size_t len);

        static bool read_boolean(const uint8_t *data, size_t len, size_t &offset);
        static uint32_t read_uint32(const uint8_t *data, size_t len, size_t &offset);
        static uint64_t read_uint64(const uint8_t *data, size_t len, size_t &offset);
        static std::optional<std::string> read_string(const uint8_t *data, size_t len, size_t &offset);
        static std::optional<std::vector<uint8_t> > read_mpint(const uint8_t *data, size_t len, size_t &offset);
        static std::optional<std::string> read_name_list(const uint8_t *data, size_t len, size_t &offset);
        static std::optional<std::vector<uint8_t> > read_raw(const uint8_t *data, size_t len, size_t &offset, size_t count);

        static ByteBuffer encode_kex_init(const SshKexInitMessage &msg);
        static std::optional<SshKexInitMessage> decode_kex_init(const uint8_t *data, size_t len);

        static ByteBuffer encode_disconnect(const SshDisconnectMessage &msg);
        static std::optional<SshDisconnectMessage> decode_disconnect(const uint8_t *data, size_t len);

        static ByteBuffer encode_service_request(const SshServiceRequestMessage &msg);
        static std::optional<SshServiceRequestMessage> decode_service_request(const uint8_t *data, size_t len);

        static ByteBuffer encode_service_accept(const SshServiceAcceptMessage &msg);
        static std::optional<SshServiceAcceptMessage> decode_service_accept(const uint8_t *data, size_t len);

        static ByteBuffer encode_kex_ecdh_init(const SshKexEcdhInitMessage &msg);
        static std::optional<SshKexEcdhInitMessage> decode_kex_ecdh_init(const uint8_t *data, size_t len);

        static ByteBuffer encode_kex_ecdh_reply(const SshKexEcdhReplyMessage &msg);
        static std::optional<SshKexEcdhReplyMessage> decode_kex_ecdh_reply(const uint8_t *data, size_t len);

        static ByteBuffer encode_userauth_request(const SshUserauthRequestMessage &msg);
        static std::optional<SshUserauthRequestMessage> decode_userauth_request(const uint8_t *data, size_t len);

        static ByteBuffer encode_userauth_failure(const SshUserauthFailureMessage &msg);
        static std::optional<SshUserauthFailureMessage> decode_userauth_failure(const uint8_t *data, size_t len);

        static ByteBuffer encode_userauth_banner(const SshUserauthBannerMessage &msg);
        static std::optional<SshUserauthBannerMessage> decode_userauth_banner(const uint8_t *data, size_t len);

        static ByteBuffer encode_userauth_pk_ok(const SshUserauthPkOkMessage &msg);
        static std::optional<SshUserauthPkOkMessage> decode_userauth_pk_ok(const uint8_t *data, size_t len);

        static ByteBuffer encode_userauth_info_request(const SshUserauthInfoRequestMessage &msg);
        static std::optional<SshUserauthInfoRequestMessage> decode_userauth_info_request(const uint8_t *data, size_t len);

        static ByteBuffer encode_userauth_info_response(const SshUserauthInfoResponseMessage &msg);
        static std::optional<SshUserauthInfoResponseMessage> decode_userauth_info_response(const uint8_t *data, size_t len);

        static ByteBuffer encode_channel_open(const SshChannelOpenMessage &msg);
        static std::optional<SshChannelOpenMessage> decode_channel_open(const uint8_t *data, size_t len);

        static ByteBuffer encode_channel_open_confirmation(const SshChannelOpenConfirmationMessage &msg);
        static std::optional<SshChannelOpenConfirmationMessage> decode_channel_open_confirmation(const uint8_t *data, size_t len);

        static ByteBuffer encode_channel_open_failure(const SshChannelOpenFailureMessage &msg);
        static std::optional<SshChannelOpenFailureMessage> decode_channel_open_failure(const uint8_t *data, size_t len);

        static ByteBuffer encode_channel_window_adjust(const SshChannelWindowAdjustMessage &msg);
        static std::optional<SshChannelWindowAdjustMessage> decode_channel_window_adjust(const uint8_t *data, size_t len);

        static ByteBuffer encode_channel_data(const SshChannelDataMessage &msg);
        static std::optional<SshChannelDataMessage> decode_channel_data(const uint8_t *data, size_t len);

        static ByteBuffer encode_channel_extended_data(const SshChannelExtendedDataMessage &msg);
        static std::optional<SshChannelExtendedDataMessage> decode_channel_extended_data(const uint8_t *data, size_t len);

        static ByteBuffer encode_channel_eof(const SshChannelEofMessage &msg);
        static std::optional<SshChannelEofMessage> decode_channel_eof(const uint8_t *data, size_t len);

        static ByteBuffer encode_channel_close(const SshChannelCloseMessage &msg);
        static std::optional<SshChannelCloseMessage> decode_channel_close(const uint8_t *data, size_t len);

        static ByteBuffer encode_channel_request(const SshChannelRequestMessage &msg);
        static std::optional<SshChannelRequestMessage> decode_channel_request(const uint8_t *data, size_t len);

        static ByteBuffer encode_channel_success(const SshChannelSuccessMessage &msg);
        static std::optional<SshChannelSuccessMessage> decode_channel_success(const uint8_t *data, size_t len);

        static ByteBuffer encode_channel_failure(const SshChannelFailureMessage &msg);
        static std::optional<SshChannelFailureMessage> decode_channel_failure(const uint8_t *data, size_t len);

        static ByteBuffer encode_global_request(const SshGlobalRequestMessage &msg);
        static std::optional<SshGlobalRequestMessage> decode_global_request(const uint8_t *data, size_t len);

        static ByteBuffer encode_newkeys();
        static ByteBuffer encode_userauth_success();

        static ByteBuffer encode_ignore(const std::vector<uint8_t> &data);
        static ByteBuffer encode_debug(const SshDebugMessage &msg);
        static ByteBuffer encode_unimplemented(uint32_t seq);
    };
}

#endif
