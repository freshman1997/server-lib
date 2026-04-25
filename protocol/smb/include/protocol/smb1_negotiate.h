#ifndef __NET_SMB_PROTOCOL_SMB1_NEGOTIATE_H__
#define __NET_SMB_PROTOCOL_SMB1_NEGOTIATE_H__

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include "buffer/byte_buffer.h"

namespace yuan::net::smb
{
    constexpr uint32_t SMB1_MAGIC = 0xFF534D42;
    constexpr uint8_t SMB1_COMMAND_NEGOTIATE = 0x72;

    struct Smb1NegotiateRequest
    {
        uint8_t command = 0;
        std::vector<std::string> dialects;
        bool supports_smb2 = false;
    };

    class Smb1Negotiate
    {
    public:
        static bool is_smb1_negotiate(const uint8_t *data, size_t len)
        {
            if (len < 5) {
                return false;
            }
            return data[0] == 0xFF && data[1] == 'S' && data[2] == 'M' && data[3] == 'B' && data[4] == SMB1_COMMAND_NEGOTIATE;
        }

        static std::optional<Smb1NegotiateRequest> decode(const uint8_t *data, size_t len)
        {
            if (!is_smb1_negotiate(data, len)) {
                return std::nullopt;
            }

            if (len < 36) {
                return std::nullopt;
            }

            Smb1NegotiateRequest req;
            req.command = data[4];

            size_t offset = 32;
            uint8_t word_count = data[offset];
            offset += 1 + word_count * 2;

            if (offset + 2 > len) {
                return std::nullopt;
            }

            uint16_t byte_count = static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
            offset += 2;

            size_t end = offset + byte_count;
            if (end > len) {
                end = len;
            }
            if ((offset >= end || data[offset] != 0x02) && len > 32) {
                for (size_t i = 32; i < len; ++i) {
                    if (data[i] == 0x02) {
                        offset = i;
                        end = len;
                        break;
                    }
                }
            }

            while (offset < end) {
                if (data[offset] != 0x02) {
                    break;
                }
                offset++;
                if (offset >= end) {
                    break;
                }
                const char *str_start = reinterpret_cast<const char *>(data + offset);
                size_t str_len = 0;
                while (offset + str_len < end && str_start[str_len] != '\0') {
                    str_len++;
                }
                std::string dialect(str_start, str_len);
                offset += str_len + 1;
                req.dialects.push_back(std::move(dialect));
            }

            for (const auto &d : req.dialects) {
                if (d == "SMB 2.002" || d == "SMB 2.???" || d == "SMB 2.1") {
                    req.supports_smb2 = true;
                    break;
                }
            }

            return req;
        }

        static ByteBuffer build_smb2_negotiate_redirect(const std::string &server_name)
        {
            ByteBuffer buf(128);

            buf.append_u8(0xFF);
            buf.append("SMB", 3);
            buf.append_u8(SMB1_COMMAND_NEGOTIATE);

            uint8_t status_bytes[4] = { 0x00, 0x00, 0x00, 0x00 };
            buf.append(status_bytes, 4);

            buf.append_u8(0x98);

            uint8_t flags2_bytes[2] = { 0x43, 0xC8 };
            buf.append(flags2_bytes, 2);

            uint8_t pid_high[2] = { 0x00, 0x00 };
            buf.append(pid_high, 2);

            uint8_t signature[8] = {};
            buf.append(signature, 8);

            uint8_t reserved[2] = { 0x00, 0x00 };
            buf.append(reserved, 2);

            uint8_t tid[2] = { 0x00, 0x00 };
            buf.append(tid, 2);

            uint8_t pid[2] = { 0x00, 0x00 };
            buf.append(pid, 2);

            uint8_t uid[2] = { 0x00, 0x00 };
            buf.append(uid, 2);

            uint8_t mid[2] = { 0x00, 0x00 };
            buf.append(mid, 2);

            buf.append_u8(17);

            uint8_t dialect_index[2] = { 0x00, 0x00 };
            buf.append(dialect_index, 2);

            buf.append_u8(0x01);

            uint8_t max_mpx[2] = { 0x32, 0x00 };
            buf.append(max_mpx, 2);

            uint8_t max_vcs[2] = { 0x01, 0x00 };
            buf.append(max_vcs, 2);

            uint8_t max_buf[4] = { 0xFF, 0xFF, 0x00, 0x00 };
            buf.append(max_buf, 4);

            uint8_t max_raw[4] = { 0xFF, 0xFF, 0x00, 0x00 };
            buf.append(max_raw, 4);

            uint8_t session_key[4] = { 0x00, 0x00, 0x00, 0x00 };
            buf.append(session_key, 4);

            uint8_t capabilities[4] = { 0x00, 0x00, 0x00, 0x80 };
            buf.append(capabilities, 4);

            uint8_t system_time[8] = {};
            buf.append(system_time, 8);

            uint8_t timezone[2] = { 0x00, 0x00 };
            buf.append(timezone, 2);

            buf.append_u8(0x00);

            uint8_t byte_count_val[2] = { 0x10, 0x00 };
            buf.append(byte_count_val, 2);

            uint8_t guid[16] = {};
            if (server_name.size() > 0) {
                for (size_t i = 0; i < 16; i++) {
                    guid[i] = static_cast<uint8_t>(server_name[i % server_name.size()]);
                }
            }
            buf.append(guid, 16);

            return buf;
        }
    };
}
#endif
