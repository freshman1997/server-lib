#include "sftp/ssh_sftp_codec.h"
#include <cstring>

namespace yuan::net::ssh
{
    static void write_u32_be(ByteBuffer & buf, uint32_t val)
    {
        buf.append_u32(val);
    }

    static void write_u64_be(ByteBuffer & buf, uint64_t val)
    {
        buf.append_u64(val);
    }

    static void write_string(ByteBuffer & buf, const std::string & s)
    {
        write_u32_be(buf, static_cast<uint32_t>(s.size()));
        if (!s.empty()) {
            buf.append(s.data(), s.size());
        }
    }

    static void write_raw(ByteBuffer & buf, const uint8_t * data, size_t len)
    {
        if (len > 0) {
            buf.append(data, len);
        }
    }

    static bool read_u32_be(const uint8_t * data, size_t len, size_t & offset, uint32_t & out)
    {
        if (offset + 4 > len)
            return false;
        out = (static_cast<uint32_t>(data[offset]) << 24) |
              (static_cast<uint32_t>(data[offset + 1]) << 16) |
              (static_cast<uint32_t>(data[offset + 2]) << 8) |
              static_cast<uint32_t>(data[offset + 3]);
        offset += 4;
        return true;
    }

    static bool read_u64_be(const uint8_t * data, size_t len, size_t & offset, uint64_t & out)
    {
        if (offset + 8 > len)
            return false;
        out = 0;
        for (int i = 0; i < 8; ++i) {
            out = (out << 8) | static_cast<uint64_t>(data[offset + i]);
        }
        offset += 8;
        return true;
    }

    static bool read_string_data(const uint8_t * data, size_t len, size_t & offset, std::string & out)
    {
        uint32_t slen = 0;
        if (!read_u32_be(data, len, offset, slen))
            return false;
        if (offset + slen > len)
            return false;
        out.assign(reinterpret_cast<const char *>(data + offset), slen);
        offset += slen;
        return true;
    }

    static bool read_raw_data(const uint8_t * data, size_t len, size_t & offset, uint32_t count, std::vector<uint8_t> & out)
    {
        if (offset + count > len)
            return false;
        out.assign(data + offset, data + offset + count);
        offset += count;
        return true;
    }

    std::optional<SftpPacket> SshSftpCodec::decode(const uint8_t * data, size_t len)
    {
        if (len < 9)
            return std::nullopt;

        size_t offset = 0;
        uint32_t packet_len = 0;
        if (!read_u32_be(data, len, offset, packet_len))
            return std::nullopt;

        if (offset + packet_len > len)
            return std::nullopt;

        SftpPacket pkt;
        pkt.type = static_cast<SftpPacketType>(data[offset++]);

        if (pkt.type != SftpPacketType::SSH_FXP_INIT && pkt.type != SftpPacketType::SSH_FXP_VERSION) {
            if (!read_u32_be(data, len, offset, pkt.request_id))
                return std::nullopt;
        }

        size_t payload_start = offset;
        size_t payload_end = 4 + packet_len;
        if (payload_end > len)
            payload_end = len;
        if (payload_end > payload_start) {
            pkt.payload.assign(data + payload_start, data + payload_end);
        }

        return pkt;
    }

    ByteBuffer SshSftpCodec::encode(const SftpPacket & packet)
    {
        ByteBuffer buf;

        size_t payload_size = 1;
        if (packet.type != SftpPacketType::SSH_FXP_INIT && packet.type != SftpPacketType::SSH_FXP_VERSION) {
            payload_size += 4;
        }
        payload_size += packet.payload.size();

        write_u32_be(buf, static_cast<uint32_t>(payload_size));
        buf.append(static_cast<uint8_t>(packet.type));

        if (packet.type != SftpPacketType::SSH_FXP_INIT && packet.type != SftpPacketType::SSH_FXP_VERSION) {
            write_u32_be(buf, packet.request_id);
        }

        if (!packet.payload.empty()) {
            write_raw(buf, packet.payload.data(), packet.payload.size());
        }

        return buf;
    }

    ByteBuffer SshSftpCodec::encode_version(uint32_t version)
    {
        ByteBuffer buf;
        size_t payload_size = 1 + 4;
        write_u32_be(buf, static_cast<uint32_t>(payload_size));
        buf.append(static_cast<uint8_t>(SftpPacketType::SSH_FXP_VERSION));
        write_u32_be(buf, version);
        return buf;
    }

    ByteBuffer SshSftpCodec::encode_status(uint32_t request_id, SftpStatus code,
                                           const std::string & message, const std::string & language)
    {
        ByteBuffer payload;
        write_u32_be(payload, static_cast<uint32_t>(code));
        write_string(payload, message);
        write_string(payload, language);

        SftpPacket pkt;
        pkt.type = SftpPacketType::SSH_FXP_STATUS;
        pkt.request_id = request_id;
        pkt.payload.assign(payload.read_ptr(), payload.read_ptr() + payload.readable_bytes());
        return encode(pkt);
    }

    ByteBuffer SshSftpCodec::encode_handle(uint32_t request_id, const std::string & handle)
    {
        ByteBuffer payload;
        write_string(payload, handle);

        SftpPacket pkt;
        pkt.type = SftpPacketType::SSH_FXP_HANDLE;
        pkt.request_id = request_id;
        pkt.payload.assign(payload.read_ptr(), payload.read_ptr() + payload.readable_bytes());
        return encode(pkt);
    }

    ByteBuffer SshSftpCodec::encode_data(uint32_t request_id, const uint8_t * data, size_t len)
    {
        ByteBuffer payload;
        write_u32_be(payload, static_cast<uint32_t>(len));
        if (len > 0) {
            write_raw(payload, data, len);
        }

        SftpPacket pkt;
        pkt.type = SftpPacketType::SSH_FXP_DATA;
        pkt.request_id = request_id;
        pkt.payload.assign(payload.read_ptr(), payload.read_ptr() + payload.readable_bytes());
        return encode(pkt);
    }

    ByteBuffer SshSftpCodec::encode_name(uint32_t request_id, const std::vector<SftpNameEntry> & entries)
    {
        ByteBuffer payload;
        write_u32_be(payload, static_cast<uint32_t>(entries.size()));
        for (const auto &entry : entries) {
            write_string(payload, entry.filename);
            write_string(payload, entry.longname);
            encode_attrs_fields(payload, entry.attrs);
        }

        SftpPacket pkt;
        pkt.type = SftpPacketType::SSH_FXP_NAME;
        pkt.request_id = request_id;
        pkt.payload.assign(payload.read_ptr(), payload.read_ptr() + payload.readable_bytes());
        return encode(pkt);
    }

    ByteBuffer SshSftpCodec::encode_attrs(uint32_t request_id, const SftpFileAttrs & attrs)
    {
        ByteBuffer payload;
        encode_attrs_fields(payload, attrs);

        SftpPacket pkt;
        pkt.type = SftpPacketType::SSH_FXP_ATTRS;
        pkt.request_id = request_id;
        pkt.payload.assign(payload.read_ptr(), payload.read_ptr() + payload.readable_bytes());
        return encode(pkt);
    }

    void SshSftpCodec::encode_attrs_fields(ByteBuffer & buf, const SftpFileAttrs & attrs)
    {
        write_u32_be(buf, attrs.flags);
        if (attrs.flags & SSH_FILEXFER_ATTR_SIZE) {
            write_u64_be(buf, attrs.size);
        }
        if (attrs.flags & SSH_FILEXFER_ATTR_UIDGID) {
            write_u32_be(buf, attrs.uid);
            write_u32_be(buf, attrs.gid);
        }
        if (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
            write_u32_be(buf, attrs.permissions);
        }
        if (attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) {
            write_u32_be(buf, attrs.atime);
            write_u32_be(buf, attrs.mtime);
        }
        if (attrs.flags & SSH_FILEXFER_ATTR_EXTENDED) {
            write_u32_be(buf, static_cast<uint32_t>(attrs.extended_names.size()));
            for (size_t i = 0; i < attrs.extended_names.size(); ++i) {
                write_string(buf, attrs.extended_names[i]);
                write_string(buf, attrs.extended_values[i]);
            }
        }
    }

    std::optional<SftpFileAttrs> SshSftpCodec::decode_attrs(const uint8_t * data, size_t len, size_t & offset)
    {
        SftpFileAttrs attrs;
        if (!read_u32_be(data, len, offset, attrs.flags))
            return std::nullopt;

        if (attrs.flags & SSH_FILEXFER_ATTR_SIZE) {
            if (!read_u64_be(data, len, offset, attrs.size))
                return std::nullopt;
        }
        if (attrs.flags & SSH_FILEXFER_ATTR_UIDGID) {
            if (!read_u32_be(data, len, offset, attrs.uid))
                return std::nullopt;
            if (!read_u32_be(data, len, offset, attrs.gid))
                return std::nullopt;
        }
        if (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
            if (!read_u32_be(data, len, offset, attrs.permissions))
                return std::nullopt;
        }
        if (attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) {
            if (!read_u32_be(data, len, offset, attrs.atime))
                return std::nullopt;
            if (!read_u32_be(data, len, offset, attrs.mtime))
                return std::nullopt;
        }
        if (attrs.flags & SSH_FILEXFER_ATTR_EXTENDED) {
            uint32_t count = 0;
            if (!read_u32_be(data, len, offset, count))
                return std::nullopt;
            for (uint32_t i = 0; i < count; ++i) {
                std::string name, value;
                if (!read_string_data(data, len, offset, name))
                    return std::nullopt;
                if (!read_string_data(data, len, offset, value))
                    return std::nullopt;
                attrs.extended_names.push_back(std::move(name));
                attrs.extended_values.push_back(std::move(value));
            }
        }

        return attrs;
    }

    std::optional<std::string> SshSftpCodec::read_handle(const uint8_t * data, size_t len, size_t & offset)
    {
        std::string handle;
        if (!read_string_data(data, len, offset, handle))
            return std::nullopt;
        return handle;
    }
}
