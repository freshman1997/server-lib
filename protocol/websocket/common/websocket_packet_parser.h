#ifndef __NET_WEBSOCKET_COMMON_PACKET_PARSER_H__
#define __NET_WEBSOCKET_COMMON_PACKET_PARSER_H__
#include "buffer/buffer.h"
#include "websocket_protocol.h"

namespace net::websocket 
{
    class WebSocketConnection;

    class WebSocketPacketParser
    {
    public:
        WebSocketPacketParser();

        bool unpack(WebSocketConnection *conn);

        bool pack(WebSocketConnection *conn, Buffer *buff, uint8_t type);

        void update_mask();

        void use_mask(bool use);
        
    private:
        bool read_chunk(ProtoChunk *chunk, Buffer *buff);

        bool merge_frame(std::vector<ProtoChunk> *chunks);

        void apply_mask(Buffer *buff, uint32_t buffSize, uint8_t *mask, uint32_t len);

        void apply_mask(Buffer *data, Buffer *buff, uint32_t buffSize);

        bool pack_header(Buffer *buff, uint8_t type, uint32_t buffSize, bool isEnd);

        bool pack_frame(Buffer *data, Buffer *buff, uint32_t size);

    private:
        bool use_mask_;
        uint8_t mask_[4];
        Buffer frame_buffer_;
    };
}

#endif
