#include "protocol/smb2_codec.h"
#include <cstring>
#include <chrono>
#include <codecvt>
#include <locale>

namespace yuan::net::smb
{
    uint16_t Smb2Codec::read_le16(const uint8_t * p)
    {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }

    uint32_t Smb2Codec::read_le32(const uint8_t * p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    uint64_t Smb2Codec::read_le64(const uint8_t * p)
    {
        return static_cast<uint64_t>(read_le32(p)) | (static_cast<uint64_t>(read_le32(p + 4)) << 32);
    }

    void Smb2Codec::write_le16(ByteBuffer & buf, uint16_t v)
    {
        uint8_t b[2] = {
            static_cast<uint8_t>(v & 0xFF),
            static_cast<uint8_t>((v >> 8) & 0xFF)
        };
        buf.append(b, 2);
    }

    void Smb2Codec::write_le32(ByteBuffer & buf, uint32_t v)
    {
        uint8_t b[4] = {
            static_cast<uint8_t>(v & 0xFF),
            static_cast<uint8_t>((v >> 8) & 0xFF),
            static_cast<uint8_t>((v >> 16) & 0xFF),
            static_cast<uint8_t>((v >> 24) & 0xFF)
        };
        buf.append(b, 4);
    }

    void Smb2Codec::write_le64(ByteBuffer & buf, uint64_t v)
    {
        uint8_t b[8] = {
            static_cast<uint8_t>(v & 0xFF),
            static_cast<uint8_t>((v >> 8) & 0xFF),
            static_cast<uint8_t>((v >> 16) & 0xFF),
            static_cast<uint8_t>((v >> 24) & 0xFF),
            static_cast<uint8_t>((v >> 32) & 0xFF),
            static_cast<uint8_t>((v >> 40) & 0xFF),
            static_cast<uint8_t>((v >> 48) & 0xFF),
            static_cast<uint8_t>((v >> 56) & 0xFF)
        };
        buf.append(b, 8);
    }

    bool Smb2Codec::is_smb2_header(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE)
            return false;
        return data[0] == 0xFE && data[1] == 'S' && data[2] == 'M' && data[3] == 'B';
    }

    bool Smb2Codec::is_transform_header(const uint8_t * data, size_t len)
    {
        if (len < SMB2_TRANSFORM_HEADER_SIZE)
            return false;
        return data[0] == 0xFD && data[1] == 'S' && data[2] == 'M' && data[3] == 'B';
    }

    std::optional<Smb2Header> Smb2Codec::decode_header(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE)
            return std::nullopt;

        Smb2Header hdr;
        if (!is_smb2_header(data, len)) {
            return std::nullopt;
        }
        hdr.protocol_id = SMB2_PROTOCOL_ID;
        hdr.structure_size = read_le16(data + 4);
        hdr.credit_charge = read_le16(data + 6);
        hdr.status = read_le32(data + 8);
        hdr.command = read_le16(data + 12);
        hdr.credit_request = read_le16(data + 14);
        hdr.flags = read_le32(data + 16);
        hdr.next_command = read_le32(data + 20);
        hdr.message_id = read_le64(data + 24);
        hdr.reserved = read_le32(data + 32);
        hdr.tree_id = read_le32(data + 36);
        hdr.session_id = read_le64(data + 40);
        std::memcpy(hdr.signature, data + 48, SMB2_SIGNATURE_SIZE);
        return hdr;
    }

    ByteBuffer Smb2Codec::encode_header(const Smb2Header & header)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE);
        uint8_t protocol_id[4] = { 0xFE, 'S', 'M', 'B' };
        buf.append(protocol_id, sizeof(protocol_id));
        write_le16(buf, header.structure_size);
        write_le16(buf, header.credit_charge);
        write_le32(buf, header.status);
        write_le16(buf, header.command);
        write_le16(buf, header.credit_request);
        write_le32(buf, header.flags);
        write_le32(buf, header.next_command);
        write_le64(buf, header.message_id);
        write_le32(buf, header.reserved);
        write_le32(buf, header.tree_id);
        write_le64(buf, header.session_id);
        buf.append(header.signature, SMB2_SIGNATURE_SIZE);
        return buf;
    }

    std::optional<Smb2TransformHeader> Smb2Codec::decode_transform_header(const uint8_t * data, size_t len)
    {
        if (len < SMB2_TRANSFORM_HEADER_SIZE)
            return std::nullopt;

        Smb2TransformHeader hdr;
        if (!is_transform_header(data, len)) {
            return std::nullopt;
        }
        hdr.protocol_id = SMB2_TRANSFORM_PROTOCOL_ID;
        std::memcpy(hdr.signature, data + 4, SMB2_SIGNATURE_SIZE);
        std::memcpy(hdr.nonce, data + 20, 16);
        hdr.original_message_size = read_le32(data + 36);
        hdr.reserved = read_le16(data + 40);
        hdr.encryption_algorithm = read_le16(data + 42);
        hdr.session_id = read_le64(data + 44);
        return hdr;
    }

    ByteBuffer Smb2Codec::encode_transform_header(const Smb2TransformHeader & header)
    {
        ByteBuffer buf(SMB2_TRANSFORM_HEADER_SIZE);
        uint8_t protocol_id[4] = { 0xFD, 'S', 'M', 'B' };
        buf.append(protocol_id, sizeof(protocol_id));
        buf.append(header.signature, SMB2_SIGNATURE_SIZE);
        buf.append(header.nonce, 16);
        write_le32(buf, header.original_message_size);
        write_le16(buf, header.reserved);
        write_le16(buf, header.encryption_algorithm);
        write_le64(buf, header.session_id);
        return buf;
    }

    std::optional<Smb2NegotiateRequest> Smb2Codec::decode_negotiate_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 8)
            return std::nullopt;

        const uint8_t *p = data + SMB2_HEADER_SIZE;
        Smb2NegotiateRequest req;
        req.structure_size = read_le16(p);
        req.dialect_count = read_le16(p + 2);
        req.security_mode = read_le16(p + 4);
        req.reserved = read_le16(p + 6);
        size_t dialects_offset = 8;
        if (len >= SMB2_HEADER_SIZE + 36) {
            req.capabilities = read_le32(p + 8);
            std::memcpy(req.client_guid, p + 12, 16);
            req.client_start_time = read_le64(p + 28);
            dialects_offset = 36;
        }

        if (len < SMB2_HEADER_SIZE + dialects_offset + req.dialect_count * 2)
            return std::nullopt;

        const uint8_t *dialects = p + dialects_offset;
        for (uint16_t i = 0; i < req.dialect_count; ++i) {
            req.dialects.push_back(read_le16(dialects + i * 2));
        }

        size_t dialects_end = dialects_offset + req.dialect_count * 2;
        if (len > SMB2_HEADER_SIZE + dialects_end) {
            req.preauth_hash.assign(p + dialects_end, p + len);
        }

        return req;
    }

    ByteBuffer Smb2Codec::encode_negotiate_response(const Smb2Header & header, const Smb2NegotiateResponse & resp)
    {
        ByteBuffer buf(256);

        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::NEGOTIATE));
        buf.append(encode_header(hdr));

        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.security_mode);
        write_le16(buf, resp.dialect_revision);
        write_le16(buf, resp.reserved);
        buf.append(resp.server_guid, 16);
        write_le32(buf, resp.capabilities);
        write_le32(buf, resp.max_transact_size);
        write_le32(buf, resp.max_read_size);
        write_le32(buf, resp.max_write_size);
        write_le64(buf, resp.system_time);
        write_le64(buf, resp.server_start_time);

        uint16_t sec_offset = SMB2_HEADER_SIZE + 64 + 8;
        write_le16(buf, sec_offset);
        write_le16(buf, static_cast<uint16_t>(resp.security_buffer.size()));

        if (resp.dialect_revision >= static_cast<uint16_t>(DialectRevision::SMB_3_1_1)) {
            uint32_t ctx_offset = sec_offset + static_cast<uint32_t>(resp.security_buffer.size());
            ctx_offset = (ctx_offset + 7) & ~7u;
            write_le32(buf, ctx_offset);
            write_le16(buf, static_cast<uint16_t>(resp.negotiate_context.size() > 0 ? 1 : 0));
            write_le16(buf, resp.reserved2);
        } else {
            write_le32(buf, 0);
            write_le16(buf, 0);
            write_le16(buf, 0);
        }

        buf.append(resp.security_buffer.data(), resp.security_buffer.size());

        if (!resp.negotiate_context.empty()) {
            size_t pad = (8 - (buf.write_offset() & 7)) & 7;
            for (size_t i = 0; i < pad; ++i)
                buf.append_u8(0);
            buf.append(resp.negotiate_context.data(), resp.negotiate_context.size());
        }

        return buf;
    }

    std::optional<Smb2SessionSetupRequest> Smb2Codec::decode_session_setup_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 25)
            return std::nullopt;

        const uint8_t *p = data + SMB2_HEADER_SIZE;
        Smb2SessionSetupRequest req;
        req.structure_size = read_le16(p);
        req.flags = p[2];
        req.security_mode = p[3];
        req.capabilities = read_le32(p + 4);
        req.channel = read_le32(p + 8);
        req.security_buffer_offset = read_le16(p + 12);
        req.security_buffer_length = read_le16(p + 14);
        req.previous_session_id = read_le64(p + 16);

        if (req.security_buffer_offset > 0 && req.security_buffer_length > 0) {
            size_t offset = req.security_buffer_offset;
            if (offset + req.security_buffer_length <= len) {
                req.security_buffer.assign(data + offset, data + offset + req.security_buffer_length);
            }
        }

        return req;
    }

    ByteBuffer Smb2Codec::encode_session_setup_response(const Smb2Header & header, const Smb2SessionSetupResponse & resp)
    {
        ByteBuffer buf(256);

        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::SESSION_SETUP));
        hdr.session_id = header.session_id;
        buf.append(encode_header(hdr));

        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.session_flags);
        uint16_t sec_offset = SMB2_HEADER_SIZE + 8;
        write_le16(buf, sec_offset);
        write_le16(buf, static_cast<uint16_t>(resp.security_buffer.size()));

        buf.append(resp.security_buffer.data(), resp.security_buffer.size());

        return buf;
    }

    std::optional<Smb2LogoffRequest> Smb2Codec::decode_logoff_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 4)
            return std::nullopt;
        Smb2LogoffRequest req;
        const uint8_t *p = data + SMB2_HEADER_SIZE;
        req.structure_size = read_le16(p);
        req.reserved = read_le16(p + 2);
        return req;
    }

    ByteBuffer Smb2Codec::encode_logoff_response(const Smb2Header & header, const Smb2LogoffResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 4);
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::LOGOFF));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.reserved);
        return buf;
    }

    std::optional<Smb2TreeConnectRequest> Smb2Codec::decode_tree_connect_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 9)
            return std::nullopt;

        const uint8_t *p = data + SMB2_HEADER_SIZE;
        Smb2TreeConnectRequest req;
        req.structure_size = read_le16(p);
        req.reserved = read_le16(p + 2);
        req.path_offset = read_le16(p + 4);
        req.path_length = read_le16(p + 6);

        if (req.path_offset > 0 && req.path_length > 0) {
            size_t offset = req.path_offset;
            size_t char_count = req.path_length / 2;
            if (offset + req.path_length <= len) {
                req.path.resize(char_count);
                for (size_t i = 0; i < char_count; ++i) {
                    req.path[i] = read_le16(data + offset + i * 2);
                }
            }
        }

        return req;
    }

    ByteBuffer Smb2Codec::encode_tree_connect_response(const Smb2Header & header, const Smb2TreeConnectResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 16);
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::TREE_CONNECT));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        buf.append_u8(resp.share_type);
        buf.append_u8(resp.reserved);
        write_le32(buf, resp.share_flags);
        write_le32(buf, resp.capabilities);
        write_le32(buf, resp.maximal_access);
        return buf;
    }

    std::optional<Smb2TreeDisconnectRequest> Smb2Codec::decode_tree_disconnect_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 4)
            return std::nullopt;
        Smb2TreeDisconnectRequest req;
        const uint8_t *p = data + SMB2_HEADER_SIZE;
        req.structure_size = read_le16(p);
        req.reserved = read_le16(p + 2);
        return req;
    }

    ByteBuffer Smb2Codec::encode_tree_disconnect_response(const Smb2Header & header, const Smb2TreeDisconnectResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 4);
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::TREE_DISCONNECT));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.reserved);
        return buf;
    }

    std::optional<Smb2CreateRequest> Smb2Codec::decode_create_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 57)
            return std::nullopt;

        const uint8_t *p = data + SMB2_HEADER_SIZE;
        Smb2CreateRequest req;
        req.structure_size = read_le16(p);
        req.security_flags = p[2];
        req.requested_oplock_level = p[3];
        req.impersonation_level = read_le32(p + 4);
        req.smb_create_flags = read_le64(p + 8);
        req.reserved = read_le64(p + 16);
        req.desired_access = read_le32(p + 24);
        req.file_attributes = read_le32(p + 28);
        req.share_access = read_le32(p + 32);
        req.create_disposition = read_le32(p + 36);
        req.create_options = read_le32(p + 40);
        req.name_offset = read_le16(p + 44);
        req.name_length = read_le16(p + 46);
        req.create_contexts_offset = read_le32(p + 48);
        req.create_contexts_length = read_le32(p + 52);

        if (req.name_offset > 0 && req.name_length > 0) {
            size_t offset = req.name_offset;
            size_t char_count = req.name_length / 2;
            if (offset + req.name_length <= len) {
                req.buffer.resize(char_count);
                for (size_t i = 0; i < char_count; ++i) {
                    req.buffer[i] = read_le16(data + offset + i * 2);
                }
            }
        }

        if (req.create_contexts_offset > 0 && req.create_contexts_length > 0) {
            size_t offset = req.create_contexts_offset;
            if (offset + req.create_contexts_length <= len) {
                req.create_contexts.assign(data + offset, data + offset + req.create_contexts_length);
            }
        }

        return req;
    }

    ByteBuffer Smb2Codec::encode_create_response(const Smb2Header & header, const Smb2CreateResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 89 + resp.create_contexts.size());
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::CREATE));
        buf.append(encode_header(hdr));

        write_le16(buf, resp.structure_size);
        buf.append_u8(resp.oplock_level);
        buf.append_u8(resp.flags);
        write_le32(buf, resp.create_action);
        write_le64(buf, resp.creation_time);
        write_le64(buf, resp.last_access_time);
        write_le64(buf, resp.last_write_time);
        write_le64(buf, resp.change_time);
        write_le64(buf, resp.allocation_size);
        write_le64(buf, resp.end_of_file);
        write_le32(buf, resp.file_attributes);
        write_le32(buf, resp.reserved2);
        write_le64(buf, resp.file_id.persistent);
        write_le64(buf, resp.file_id.volatile_id);
        write_le32(buf, resp.create_contexts_offset);
        write_le32(buf, resp.create_contexts_length);

        if (!resp.create_contexts.empty()) {
            buf.append(resp.create_contexts.data(), resp.create_contexts.size());
        }

        return buf;
    }

    std::optional<Smb2CloseRequest> Smb2Codec::decode_close_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 24)
            return std::nullopt;

        const uint8_t *p = data + SMB2_HEADER_SIZE;
        Smb2CloseRequest req;
        req.structure_size = read_le16(p);
        req.flags = read_le16(p + 2);
        req.reserved = read_le32(p + 4);
        req.file_id.persistent = read_le64(p + 8);
        req.file_id.volatile_id = read_le64(p + 16);
        return req;
    }

    ByteBuffer Smb2Codec::encode_close_response(const Smb2Header & header, const Smb2CloseResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 60);
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::CLOSE));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.flags);
        write_le32(buf, resp.reserved);
        write_le64(buf, resp.creation_time);
        write_le64(buf, resp.last_access_time);
        write_le64(buf, resp.last_write_time);
        write_le64(buf, resp.change_time);
        write_le64(buf, resp.allocation_size);
        write_le64(buf, resp.end_of_file);
        write_le32(buf, resp.file_attributes);
        return buf;
    }

    std::optional<Smb2ReadRequest> Smb2Codec::decode_read_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 49)
            return std::nullopt;

        const uint8_t *p = data + SMB2_HEADER_SIZE;
        Smb2ReadRequest req;
        req.structure_size = read_le16(p);
        req.padding = p[2];
        req.flags = p[3];
        req.length = read_le32(p + 4);
        req.offset = read_le64(p + 8);
        req.file_id.persistent = read_le64(p + 16);
        req.file_id.volatile_id = read_le64(p + 24);
        req.minimum_count = read_le32(p + 32);
        req.channel = read_le32(p + 36);
        req.remaining_bytes = read_le32(p + 40);
        req.read_channel_info_offset = read_le16(p + 44);
        req.read_channel_info_length = read_le16(p + 46);
        return req;
    }

    ByteBuffer Smb2Codec::encode_read_response(const Smb2Header & header, const Smb2ReadResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 17 + resp.buffer.size());
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::READ));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        buf.append_u8(resp.data_offset);
        buf.append_u8(resp.reserved);
        write_le32(buf, resp.data_length);
        write_le32(buf, resp.data_remaining);
        write_le32(buf, resp.reserved2);
        if (!resp.buffer.empty()) {
            buf.append(resp.buffer.data(), resp.buffer.size());
        }
        return buf;
    }

    std::optional<Smb2WriteRequest> Smb2Codec::decode_write_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 49)
            return std::nullopt;

        const uint8_t *p = data + SMB2_HEADER_SIZE;
        Smb2WriteRequest req;
        req.structure_size = read_le16(p);
        req.data_offset = read_le16(p + 2);
        req.length = read_le32(p + 4);
        req.offset = read_le64(p + 8);
        req.file_id.persistent = read_le64(p + 16);
        req.file_id.volatile_id = read_le64(p + 24);
        req.channel = read_le32(p + 32);
        req.remaining_bytes = read_le32(p + 36);
        req.write_channel_info_offset = read_le16(p + 40);
        req.write_channel_info_length = read_le16(p + 42);
        req.flags = read_le32(p + 44);

        if (req.data_offset > 0 && req.length > 0) {
            size_t offset = req.data_offset;
            if (offset + req.length <= len) {
                req.buffer.assign(data + offset, data + offset + req.length);
            }
        }

        return req;
    }

    ByteBuffer Smb2Codec::encode_write_response(const Smb2Header & header, const Smb2WriteResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 17);
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::WRITE));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.reserved);
        write_le32(buf, resp.count);
        write_le32(buf, resp.remaining);
        write_le16(buf, resp.write_channel_info_offset);
        write_le16(buf, resp.write_channel_info_length);
        return buf;
    }

    std::optional<Smb2QueryDirectoryRequest> Smb2Codec::decode_query_directory_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 33)
            return std::nullopt;

        const uint8_t *p = data + SMB2_HEADER_SIZE;
        Smb2QueryDirectoryRequest req;
        req.structure_size = read_le16(p);
        req.file_information_class = p[2];
        req.flags = p[3];
        req.file_index = read_le32(p + 4);
        req.file_id.persistent = read_le64(p + 8);
        req.file_id.volatile_id = read_le64(p + 16);
        req.file_name_offset = read_le16(p + 24);
        req.file_name_length = read_le16(p + 26);
        req.output_buffer_length = read_le32(p + 28);

        if (req.file_name_offset > 0 && req.file_name_length > 0) {
            size_t offset = req.file_name_offset;
            size_t char_count = req.file_name_length / 2;
            if (offset + req.file_name_length <= len) {
                req.file_name.resize(char_count);
                for (size_t i = 0; i < char_count; ++i) {
                    req.file_name[i] = read_le16(data + offset + i * 2);
                }
            }
        }

        return req;
    }

    ByteBuffer Smb2Codec::encode_query_directory_response(const Smb2Header & header, const Smb2QueryDirectoryResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 9 + resp.buffer.size());
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::QUERY_DIRECTORY));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.output_buffer_offset);
        write_le32(buf, resp.output_buffer_length);
        if (!resp.buffer.empty()) {
            buf.append(resp.buffer.data(), resp.buffer.size());
        }
        return buf;
    }

    std::optional<Smb2QueryInfoRequest> Smb2Codec::decode_query_info_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 41)
            return std::nullopt;

        const uint8_t *p = data + SMB2_HEADER_SIZE;
        Smb2QueryInfoRequest req;
        req.structure_size = read_le16(p);
        req.info_type = p[2];
        req.file_info_class = p[3];
        req.output_buffer_length = read_le32(p + 4);
        req.input_buffer_offset = read_le16(p + 8);
        req.reserved = read_le16(p + 10);
        req.input_buffer_length = read_le32(p + 12);
        req.additional_information = read_le32(p + 16);
        req.flags = read_le32(p + 20);
        req.file_id.persistent = read_le64(p + 24);
        req.file_id.volatile_id = read_le64(p + 32);

        if (req.input_buffer_offset > 0 && req.input_buffer_length > 0) {
            size_t offset = req.input_buffer_offset;
            if (offset + req.input_buffer_length <= len) {
                req.input_buffer.assign(data + offset, data + offset + req.input_buffer_length);
            }
        }

        return req;
    }

    ByteBuffer Smb2Codec::encode_query_info_response(const Smb2Header & header, const Smb2QueryInfoResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 9 + resp.buffer.size());
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::QUERY_INFO));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.output_buffer_offset);
        write_le32(buf, resp.output_buffer_length);
        if (!resp.buffer.empty()) {
            buf.append(resp.buffer.data(), resp.buffer.size());
        }
        return buf;
    }

    std::optional<Smb2SetInfoRequest> Smb2Codec::decode_set_info_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 33)
            return std::nullopt;

        const uint8_t *p = data + SMB2_HEADER_SIZE;
        Smb2SetInfoRequest req;
        req.structure_size = read_le16(p);
        req.info_type = p[2];
        req.file_info_class = p[3];
        req.buffer_length = read_le32(p + 4);
        req.buffer_offset = read_le16(p + 8);
        req.reserved = read_le16(p + 10);
        req.additional_information = read_le32(p + 12);
        req.file_id.persistent = read_le64(p + 16);
        req.file_id.volatile_id = read_le64(p + 24);

        if (req.buffer_offset > 0 && req.buffer_length > 0) {
            size_t offset = req.buffer_offset;
            if (offset + req.buffer_length <= len) {
                req.buffer.assign(data + offset, data + offset + req.buffer_length);
            }
        }

        return req;
    }

    ByteBuffer Smb2Codec::encode_set_info_response(const Smb2Header & header, const Smb2SetInfoResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 9);
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::SET_INFO));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.output_buffer_offset);
        write_le32(buf, resp.output_buffer_length);
        return buf;
    }

    std::optional<Smb2LockRequest> Smb2Codec::decode_lock_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 48)
            return std::nullopt;

        const uint8_t *p = data + SMB2_HEADER_SIZE;
        Smb2LockRequest req;
        req.structure_size = read_le16(p);
        req.lock_count = read_le16(p + 2);
        req.lock_sequence = read_le32(p + 4);
        req.file_id.persistent = read_le64(p + 8);
        req.file_id.volatile_id = read_le64(p + 16);

        size_t locks_offset = 24;
        for (uint16_t i = 0; i < req.lock_count; ++i) {
            if (locks_offset + 24 > len - SMB2_HEADER_SIZE)
                break;
            Smb2LockElement el;
            el.offset = read_le64(p + locks_offset);
            el.length = read_le64(p + locks_offset + 8);
            el.flags = read_le32(p + locks_offset + 16);
            el.reserved = read_le32(p + locks_offset + 20);
            req.locks.push_back(el);
            locks_offset += 24;
        }

        return req;
    }

    ByteBuffer Smb2Codec::encode_lock_response(const Smb2Header & header, const Smb2LockResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 4);
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::LOCK));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.reserved);
        return buf;
    }

    std::optional<Smb2IoctlRequest> Smb2Codec::decode_ioctl_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 57)
            return std::nullopt;

        const uint8_t *p = data + SMB2_HEADER_SIZE;
        Smb2IoctlRequest req;
        req.structure_size = read_le16(p);
        req.reserved = read_le16(p + 2);
        req.ctl_code = read_le32(p + 4);
        req.file_id.persistent = read_le64(p + 8);
        req.file_id.volatile_id = read_le64(p + 16);
        req.input_offset = read_le32(p + 24);
        req.input_length = read_le32(p + 28);
        req.max_input_response = read_le32(p + 32);
        req.output_offset = read_le32(p + 36);
        req.output_length = read_le32(p + 40);
        req.max_output_response = read_le32(p + 44);
        req.flags = read_le32(p + 48);
        req.reserved2 = read_le32(p + 52);

        if (req.input_offset > 0 && req.input_length > 0) {
            size_t offset = req.input_offset;
            if (offset + req.input_length <= len) {
                req.input_buffer.assign(data + offset, data + offset + req.input_length);
            }
        }

        if (req.output_offset > 0 && req.output_length > 0) {
            size_t offset = req.output_offset;
            if (offset + req.output_length <= len) {
                req.output_buffer.assign(data + offset, data + offset + req.output_length);
            }
        }

        return req;
    }

    ByteBuffer Smb2Codec::encode_ioctl_response(const Smb2Header & header, const Smb2IoctlResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 49 + resp.output_buffer.size());
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::IOCTL));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.reserved);
        write_le32(buf, resp.ctl_code);
        write_le64(buf, resp.file_id.persistent);
        write_le64(buf, resp.file_id.volatile_id);
        write_le32(buf, resp.input_offset);
        write_le32(buf, resp.input_length);
        write_le32(buf, resp.output_offset);
        write_le32(buf, resp.output_length);
        write_le32(buf, resp.flags);
        write_le32(buf, resp.reserved2);
        if (!resp.output_buffer.empty()) {
            buf.append(resp.output_buffer.data(), resp.output_buffer.size());
        }
        return buf;
    }

    std::optional<Smb2EchoRequest> Smb2Codec::decode_echo_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 4)
            return std::nullopt;
        Smb2EchoRequest req;
        const uint8_t *p = data + SMB2_HEADER_SIZE;
        req.structure_size = read_le16(p);
        req.reserved = read_le16(p + 2);
        return req;
    }

    ByteBuffer Smb2Codec::encode_echo_response(const Smb2Header & header, const Smb2EchoResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 4);
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::ECHO));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.reserved);
        return buf;
    }

    std::optional<Smb2FlushRequest> Smb2Codec::decode_flush_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 24)
            return std::nullopt;
        Smb2FlushRequest req;
        const uint8_t *p = data + SMB2_HEADER_SIZE;
        req.structure_size = read_le16(p);
        req.reserved = read_le16(p + 2);
        req.reserved2 = read_le32(p + 4);
        req.file_id.persistent = read_le64(p + 8);
        req.file_id.volatile_id = read_le64(p + 16);
        return req;
    }

    ByteBuffer Smb2Codec::encode_flush_response(const Smb2Header & header, const Smb2FlushResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 4);
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::FLUSH));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.reserved);
        return buf;
    }

    std::optional<Smb2ChangeNotifyRequest> Smb2Codec::decode_change_notify_request(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 32)
            return std::nullopt;
        Smb2ChangeNotifyRequest req;
        const uint8_t *p = data + SMB2_HEADER_SIZE;
        req.structure_size = read_le16(p);
        req.flags = read_le16(p + 2);
        req.output_buffer_length = read_le32(p + 4);
        req.file_id.persistent = read_le64(p + 8);
        req.file_id.volatile_id = read_le64(p + 16);
        req.completion_filter = read_le32(p + 24);
        req.reserved = read_le32(p + 28);
        return req;
    }

    ByteBuffer Smb2Codec::encode_change_notify_response(const Smb2Header & header, const Smb2ChangeNotifyResponse & resp)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 9 + resp.buffer.size());
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::CHANGE_NOTIFY));
        buf.append(encode_header(hdr));
        write_le16(buf, resp.structure_size);
        write_le16(buf, resp.output_buffer_offset);
        write_le32(buf, resp.output_buffer_length);
        if (!resp.buffer.empty()) {
            buf.append(resp.buffer.data(), resp.buffer.size());
        }
        return buf;
    }

    std::optional<Smb2OplockBreakAckRequest> Smb2Codec::decode_oplock_break_ack(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 24)
            return std::nullopt;
        Smb2OplockBreakAckRequest req;
        const uint8_t *p = data + SMB2_HEADER_SIZE;
        req.structure_size = read_le16(p);
        req.oplock_level = p[2];
        req.reserved = p[3];
        req.reserved2 = read_le32(p + 4);
        req.file_id.persistent = read_le64(p + 8);
        req.file_id.volatile_id = read_le64(p + 16);
        return req;
    }

    ByteBuffer Smb2Codec::encode_oplock_break_notification(const Smb2Header & header, const Smb2OplockBreakNotification & notif)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 24);
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::OPLOCK_BREAK));
        hdr.flags = SMB2_FLAGS_SERVER_TO_REDIR;
        hdr.credit_charge = 0;
        buf.append(encode_header(hdr));
        write_le16(buf, notif.structure_size);
        buf.append_u8(notif.oplock_level);
        buf.append_u8(notif.reserved);
        write_le32(buf, notif.reserved2);
        write_le64(buf, notif.file_id.persistent);
        write_le64(buf, notif.file_id.volatile_id);
        return buf;
    }

    std::optional<Smb2LeaseBreakAckRequest> Smb2Codec::decode_lease_break_ack(const uint8_t * data, size_t len)
    {
        if (len < SMB2_HEADER_SIZE + 36)
            return std::nullopt;
        Smb2LeaseBreakAckRequest req;
        const uint8_t *p = data + SMB2_HEADER_SIZE;
        req.structure_size = read_le16(p);
        req.reserved = read_le16(p + 2);
        req.flags = read_le32(p + 4);
        std::memcpy(req.lease_key, p + 8, SMB2_LEASE_KEY_SIZE);
        req.lease_state = read_le32(p + 24);
        req.lease_duration = read_le64(p + 28);
        return req;
    }

    ByteBuffer Smb2Codec::encode_lease_break_notification(const Smb2Header & header, const Smb2LeaseBreakNotification & notif)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 44);
        auto hdr = make_response_header(header, static_cast<uint16_t>(Smb2Command::OPLOCK_BREAK));
        hdr.flags = SMB2_FLAGS_SERVER_TO_REDIR;
        hdr.credit_charge = 0;
        buf.append(encode_header(hdr));
        write_le16(buf, notif.structure_size);
        write_le16(buf, notif.new_epoch);
        write_le32(buf, notif.flags);
        buf.append(notif.lease_key, SMB2_LEASE_KEY_SIZE);
        write_le32(buf, notif.current_lease_state);
        write_le32(buf, notif.new_lease_state);
        write_le32(buf, notif.break_reason);
        write_le32(buf, notif.access_mask_hint);
        write_le32(buf, notif.share_mask_hint);
        return buf;
    }

    ByteBuffer Smb2Codec::build_error_response(const Smb2Header & req_header, NtStatus status)
    {
        ByteBuffer buf(SMB2_HEADER_SIZE + 9);
        auto hdr = make_response_header(req_header, req_header.command);
        hdr.status = static_cast<uint32_t>(status);
        hdr.flags |= SMB2_FLAGS_SERVER_TO_REDIR;
        buf.append(encode_header(hdr));
        write_le16(buf, 9);
        buf.append_u8(0);
        buf.append_u8(0);
        write_le32(buf, 0);
        write_le16(buf, 0);
        write_le16(buf, 0);
        return buf;
    }

    Smb2Header Smb2Codec::make_response_header(const Smb2Header & req_header, uint16_t command, uint16_t credit_grant)
    {
        Smb2Header hdr;
        hdr.protocol_id = SMB2_PROTOCOL_ID;
        hdr.structure_size = SMB2_HEADER_SIZE;
        hdr.credit_charge = 0;
        hdr.status = static_cast<uint32_t>(NtStatus::SUCCESS);
        hdr.command = command;
        hdr.credit_request = credit_grant;
        hdr.flags = SMB2_FLAGS_SERVER_TO_REDIR;
        hdr.next_command = 0;
        hdr.message_id = req_header.message_id;
        hdr.reserved = 0;
        hdr.tree_id = req_header.tree_id;
        hdr.session_id = req_header.session_id;
        std::memset(hdr.signature, 0, SMB2_SIGNATURE_SIZE);
        return hdr;
    }

    std::vector<Smb2CreateContext> Smb2Codec::parse_create_contexts(const std::vector<uint8_t> & data)
    {
        std::vector<Smb2CreateContext> contexts;
        if (data.size() < 16)
            return contexts;

        size_t offset = 0;
        while (offset + 16 <= data.size()) {
            const uint8_t *p = data.data() + offset;
            Smb2CreateContext ctx;
            ctx.next = read_le32(p);
            ctx.name_offset = read_le16(p + 4);
            ctx.name_length = read_le16(p + 6);
            ctx.data_offset = read_le16(p + 8);
            ctx.data_length = read_le16(p + 10);
            ctx.reserved = read_le32(p + 12);

            if (ctx.name_offset > 0 && ctx.name_length > 0) {
                size_t name_start = ctx.name_offset - 16;
                if (name_start + ctx.name_length <= data.size()) {
                    ctx.name.assign(data.data() + name_start, data.data() + name_start + ctx.name_length);
                }
            }

            if (ctx.data_offset > 0 && ctx.data_length > 0) {
                size_t data_start = ctx.data_offset - 16;
                if (data_start + ctx.data_length <= data.size()) {
                    ctx.data.assign(data.data() + data_start, data.data() + data_start + ctx.data_length);
                }
            }

            contexts.push_back(std::move(ctx));

            if (ctx.next == 0)
                break;
            offset += ctx.next;
            offset = (offset + 7) & ~7u;
        }

        return contexts;
    }

    ByteBuffer Smb2Codec::encode_create_contexts(const std::vector<Smb2CreateContext> & contexts)
    {
        ByteBuffer buf(256);
        for (size_t i = 0; i < contexts.size(); ++i) {
            const auto &ctx = contexts[i];
            write_le32(buf, i + 1 < contexts.size() ? static_cast<uint32_t>(buf.write_offset()) : 0);
            write_le16(buf, 16);
            write_le16(buf, static_cast<uint16_t>(ctx.name.size()));
            write_le16(buf, 16 + static_cast<uint16_t>((ctx.name.size() + 3) & ~3u));
            write_le16(buf, static_cast<uint16_t>(ctx.data.size()));
            write_le32(buf, 0);
            if (!ctx.name.empty()) {
                buf.append(ctx.name.data(), ctx.name.size());
                size_t pad = (4 - (ctx.name.size() & 3)) & 3;
                for (size_t j = 0; j < pad; ++j)
                    buf.append_u8(0);
            }
            if (!ctx.data.empty()) {
                buf.append(ctx.data.data(), ctx.data.size());
                size_t pad = (4 - (ctx.data.size() & 3)) & 3;
                for (size_t j = 0; j < pad; ++j)
                    buf.append_u8(0);
            }
        }
        return buf;
    }

    std::vector<NegotiateContext> Smb2Codec::parse_negotiate_contexts(const uint8_t * data, size_t len, uint16_t count)
    {
        std::vector<NegotiateContext> contexts;
        size_t offset = 0;

        for (uint16_t i = 0; i < count && offset + 8 <= len; ++i) {
            NegotiateContext ctx;
            ctx.context_type = read_le16(data + offset);
            ctx.data_length = read_le16(data + offset + 2);
            ctx.reserved = read_le32(data + offset + 4);
            offset += 8;

            if (ctx.data_length > 0 && offset + ctx.data_length <= len) {
                ctx.data.assign(data + offset, data + offset + ctx.data_length);
            }
            offset += ctx.data_length;
            offset = (offset + 7) & ~7u;

            contexts.push_back(std::move(ctx));
        }

        return contexts;
    }

    ByteBuffer Smb2Codec::encode_negotiate_contexts(const std::vector<NegotiateContext> & contexts)
    {
        ByteBuffer buf(256);
        for (const auto &ctx : contexts) {
            write_le16(buf, ctx.context_type);
            write_le16(buf, static_cast<uint16_t>(ctx.data.size()));
            write_le32(buf, 0);
            if (!ctx.data.empty()) {
                buf.append(ctx.data.data(), ctx.data.size());
                size_t pad = (8 - (ctx.data.size() & 7)) & 7;
                for (size_t j = 0; j < pad; ++j)
                    buf.append_u8(0);
            }
        }
        return buf;
    }

    std::u16string Smb2Codec::utf8_to_utf16le(const std::string & str)
    {
        std::u16string result;
        result.reserve(str.size());
        for (size_t i = 0; i < str.size();) {
            uint32_t cp = 0;
            uint8_t b = static_cast<uint8_t>(str[i]);
            if (b < 0x80) {
                cp = b;
                i += 1;
            } else if ((b & 0xE0) == 0xC0) {
                if (i + 1 >= str.size())
                    break;
                cp = (b & 0x1F) << 6;
                cp |= (static_cast<uint8_t>(str[i + 1]) & 0x3F);
                i += 2;
            } else if ((b & 0xF0) == 0xE0) {
                if (i + 2 >= str.size())
                    break;
                cp = (b & 0x0F) << 12;
                cp |= (static_cast<uint8_t>(str[i + 1]) & 0x3F) << 6;
                cp |= (static_cast<uint8_t>(str[i + 2]) & 0x3F);
                i += 3;
            } else if ((b & 0xF8) == 0xF0) {
                if (i + 3 >= str.size())
                    break;
                cp = (b & 0x07) << 18;
                cp |= (static_cast<uint8_t>(str[i + 1]) & 0x3F) << 12;
                cp |= (static_cast<uint8_t>(str[i + 2]) & 0x3F) << 6;
                cp |= (static_cast<uint8_t>(str[i + 3]) & 0x3F);
                i += 4;
            } else {
                i += 1;
                continue;
            }

            if (cp <= 0xFFFF) {
                result.push_back(static_cast<char16_t>(cp));
            } else {
                cp -= 0x10000;
                result.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
                result.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
            }
        }
        return result;
    }

    std::string Smb2Codec::utf16le_to_utf8(const std::u16string & str)
    {
        std::string result;
        result.reserve(str.size() * 3);

        for (size_t i = 0; i < str.size(); ++i) {
            uint32_t cp = str[i];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < str.size()) {
                uint32_t lo = str[i + 1];
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i++;
                }
            }

            if (cp < 0x80) {
                result.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        return result;
    }

    uint64_t Smb2Codec::filetime_now()
    {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
        return static_cast<uint64_t>((secs + 11644473600ULL) * 10000000ULL);
    }
}
