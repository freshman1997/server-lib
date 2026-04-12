#ifndef __NET_WEBSOCKET_COMMON_PACKET_PARSER_H__
#define __NET_WEBSOCKET_COMMON_PACKET_PARSER_H__
#include "buffer/byte_buffer.h"
#include "websocket_protocol.h"

namespace yuan::net::websocket 
{
    class WebSocketConnection;

    class WebSocketPacketParser
    {
    public:
        WebSocketPacketParser();
        ~WebSocketPacketParser();

        bool unpack(WebSocketConnection *conn);

        bool pack(WebSocketConnection *conn, const ::yuan::buffer::ByteBuffer &buff, uint8_t type);

        void update_mask();

        void use_mask(bool use);
        
    private:
        int read_chunk(ProtoChunk *chunk, ::yuan::buffer::ByteBuffer &buff);

        void apply_mask(const ::yuan::buffer::ByteBuffer &data, ::yuan::buffer::ByteBuffer &buff, uint32_t buffSize);

        void apply_mask(::yuan::buffer::ByteBuffer &buff, uint32_t buffSize, uint8_t *mask, uint32_t len);

        bool pack_header(::yuan::buffer::ByteBuffer &buff, uint8_t type, uint32_t buffSize, bool isEnd, bool isContinueFrame);

        bool pack_frame(const ::yuan::buffer::ByteBuffer &data, ::yuan::buffer::ByteBuffer &buff, std::size_t offset, uint32_t size);

    private:
        bool use_mask_;
        uint8_t mask_[7];
        ::yuan::buffer::ByteBuffer frame_buffer_;
    };
}

#endif
