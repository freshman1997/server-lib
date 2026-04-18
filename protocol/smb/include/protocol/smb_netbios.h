#ifndef __NET_SMB_PROTOCOL_SMB_NETBIOS_H__
#define __NET_SMB_PROTOCOL_SMB_NETBIOS_H__

#include <cstdint>
#include <optional>
#include <vector>
#include "buffer/byte_buffer.h"

using ByteBuffer = ::yuan::buffer::ByteBuffer;

namespace yuan::net::smb
{
    struct NetBiosHeader
    {
        uint8_t message_type = 0;
        uint32_t length = 0;
    };

    class SmbNetbios
    {
    public:
        static std::optional<NetBiosHeader> decode(const uint8_t *data, size_t len);
        static ByteBuffer encode(uint32_t length);
        static std::optional<std::vector<ByteBuffer> > split_messages(ByteBuffer &buf);
    };
}
#endif
