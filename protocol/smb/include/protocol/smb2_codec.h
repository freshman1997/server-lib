#ifndef __NET_SMB_PROTOCOL_SMB2_CODEC_H__
#define __NET_SMB_PROTOCOL_SMB2_CODEC_H__

#include "protocol/smb2_constants.h"
#include "protocol/smb2_structures.h"
#include "buffer/byte_buffer.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::smb
{
    using ByteBuffer = ::yuan::buffer::ByteBuffer;

    class Smb2Codec
    {
    public:
        static uint16_t read_le16(const uint8_t *p);
        static uint32_t read_le32(const uint8_t *p);
        static uint64_t read_le64(const uint8_t *p);

        static void write_le16(ByteBuffer &buf, uint16_t v);
        static void write_le32(ByteBuffer &buf, uint32_t v);
        static void write_le64(ByteBuffer &buf, uint64_t v);

        static bool is_smb2_header(const uint8_t *data, size_t len);
        static bool is_transform_header(const uint8_t *data, size_t len);

        static std::optional<Smb2Header> decode_header(const uint8_t *data, size_t len);
        static ByteBuffer encode_header(const Smb2Header &header);

        static std::optional<Smb2TransformHeader> decode_transform_header(const uint8_t *data, size_t len);
        static ByteBuffer encode_transform_header(const Smb2TransformHeader &header);

        static std::optional<Smb2NegotiateRequest> decode_negotiate_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_negotiate_response(const Smb2Header &header, const Smb2NegotiateResponse &resp);

        static std::optional<Smb2SessionSetupRequest> decode_session_setup_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_session_setup_response(const Smb2Header &header, const Smb2SessionSetupResponse &resp);

        static std::optional<Smb2LogoffRequest> decode_logoff_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_logoff_response(const Smb2Header &header, const Smb2LogoffResponse &resp);

        static std::optional<Smb2TreeConnectRequest> decode_tree_connect_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_tree_connect_response(const Smb2Header &header, const Smb2TreeConnectResponse &resp);

        static std::optional<Smb2TreeDisconnectRequest> decode_tree_disconnect_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_tree_disconnect_response(const Smb2Header &header, const Smb2TreeDisconnectResponse &resp);

        static std::optional<Smb2CreateRequest> decode_create_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_create_response(const Smb2Header &header, const Smb2CreateResponse &resp);

        static std::optional<Smb2CloseRequest> decode_close_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_close_response(const Smb2Header &header, const Smb2CloseResponse &resp);

        static std::optional<Smb2ReadRequest> decode_read_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_read_response(const Smb2Header &header, const Smb2ReadResponse &resp);

        static std::optional<Smb2WriteRequest> decode_write_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_write_response(const Smb2Header &header, const Smb2WriteResponse &resp);

        static std::optional<Smb2QueryDirectoryRequest> decode_query_directory_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_query_directory_response(const Smb2Header &header, const Smb2QueryDirectoryResponse &resp);

        static std::optional<Smb2QueryInfoRequest> decode_query_info_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_query_info_response(const Smb2Header &header, const Smb2QueryInfoResponse &resp);

        static std::optional<Smb2SetInfoRequest> decode_set_info_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_set_info_response(const Smb2Header &header, const Smb2SetInfoResponse &resp);

        static std::optional<Smb2LockRequest> decode_lock_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_lock_response(const Smb2Header &header, const Smb2LockResponse &resp);

        static std::optional<Smb2IoctlRequest> decode_ioctl_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_ioctl_response(const Smb2Header &header, const Smb2IoctlResponse &resp);

        static std::optional<Smb2EchoRequest> decode_echo_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_echo_response(const Smb2Header &header, const Smb2EchoResponse &resp);

        static std::optional<Smb2FlushRequest> decode_flush_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_flush_response(const Smb2Header &header, const Smb2FlushResponse &resp);

        static std::optional<Smb2ChangeNotifyRequest> decode_change_notify_request(const uint8_t *data, size_t len);
        static ByteBuffer encode_change_notify_response(const Smb2Header &header, const Smb2ChangeNotifyResponse &resp);

        static std::optional<Smb2OplockBreakAckRequest> decode_oplock_break_ack(const uint8_t *data, size_t len);
        static ByteBuffer encode_oplock_break_notification(const Smb2Header &header, const Smb2OplockBreakNotification &notif);

        static std::optional<Smb2LeaseBreakAckRequest> decode_lease_break_ack(const uint8_t *data, size_t len);
        static ByteBuffer encode_lease_break_notification(const Smb2Header &header, const Smb2LeaseBreakNotification &notif);

        static ByteBuffer build_error_response(const Smb2Header &req_header, NtStatus status);

        static Smb2Header make_response_header(const Smb2Header &req_header, uint16_t command,
                                               uint16_t credit_grant = 1);

        static std::vector<Smb2CreateContext> parse_create_contexts(const std::vector<uint8_t> &data);
        static ByteBuffer encode_create_contexts(const std::vector<Smb2CreateContext> &contexts);

        static std::vector<NegotiateContext> parse_negotiate_contexts(const uint8_t *data, size_t len,
                                                                      uint16_t count);
        static ByteBuffer encode_negotiate_contexts(const std::vector<NegotiateContext> &contexts);

        static std::u16string utf8_to_utf16le(const std::string &str);
        static std::string utf16le_to_utf8(const std::u16string &str);

        static uint64_t filetime_now();

    private:
        static constexpr size_t kSmb2HeaderOffset = 0;
        static constexpr size_t kSmb2HeaderSize = 64;
    };
}
#endif
