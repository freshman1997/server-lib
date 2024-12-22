#ifndef __NET_WEBSOCKET_COMMON_PACKET_PARSER_H__
#define __NET_WEBSOCKET_COMMON_PACKET_PARSER_H__
#include "buffer/buffer.h"
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

        bool pack(WebSocketConnection *conn, buffer::Buffer *buff, uint8_t type);

        void update_mask();

        void use_mask(bool use);
        
    private:
        int read_chunk(ProtoChunk *chunk, buffer::Buffer *buff);

        void apply_mask(buffer::Buffer *buff, uint32_t buffSize, uint8_t *mask, uint32_t len);

        void apply_mask(buffer::Buffer *data, buffer::Buffer *buff, uint32_t buffSize);

        bool pack_header(buffer::Buffer *buff, uint8_t type, uint32_t buffSize, bool isEnd, bool isContinueFrame);

        bool pack_frame(buffer::Buffer *data, buffer::Buffer *buff, uint32_t size);

        buffer::Buffer * get_frame_buffer();

    private:
        bool use_mask_;
        uint8_t mask_[4];
        buffer::Buffer *frame_buffer_;
    };
}

#endif
