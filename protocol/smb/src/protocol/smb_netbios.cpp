#include "protocol/smb_netbios.h"
#include <cstring>

namespace yuan::net::smb
{
    using ::yuan::buffer::ByteBuffer;

    std::optional<NetBiosHeader> SmbNetbios::decode(const uint8_t * data, size_t len)
    {
        if (len < 4) {
            return std::nullopt;
        }

        NetBiosHeader hdr;
        hdr.message_type = data[0];
        hdr.length = (static_cast<uint32_t>(data[1]) << 16) |
                     (static_cast<uint32_t>(data[2]) << 8) |
                     static_cast<uint32_t>(data[3]);
        return hdr;
    }

    ByteBuffer SmbNetbios::encode(uint32_t length)
    {
        ByteBuffer buf(4);
        buf.append_u8(0x00);
        buf.append_u8(static_cast<uint8_t>((length >> 16) & 0xFF));
        buf.append_u8(static_cast<uint8_t>((length >> 8) & 0xFF));
        buf.append_u8(static_cast<uint8_t>(length & 0xFF));
        return buf;
    }

    std::optional<std::vector<ByteBuffer> > SmbNetbios::split_messages(ByteBuffer & buf)
    {
        auto span = buf.readable_span();
        const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());
        size_t readable = span.size();
        std::vector<ByteBuffer> messages;
        size_t offset = 0;

        while (offset + 4 <= readable) {
            uint32_t length = (static_cast<uint32_t>(data[offset + 1]) << 16) |
                              (static_cast<uint32_t>(data[offset + 2]) << 8) |
                              static_cast<uint32_t>(data[offset + 3]);

            size_t total = 4 + length;
            if (offset + total > readable) {
                break;
            }

            ByteBuffer msg(total);
            msg.append(data + offset, total);
            messages.push_back(std::move(msg));
            offset += total;
        }

        buf.consume(offset);

        if (messages.empty()) {
            return std::nullopt;
        }

        return messages;
    }
}
