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
        bool unpack(WebSocketConnection *conn);

        bool pack(WebSocketConnection *conn, Buffer *buff);

    private:
    bool read_chunk(ProtoChunk *chunk, Buffer *buff);

        bool merge_frame(std::vector<ProtoChunk> *chunks);

        void frame_decode(Buffer *buff, uint8_t *mask, uint32_t len);

        Buffer * pack_header(WebSocketConnection *conn, Buffer *buff);

        bool pack_frame(WebSocketConnection *conn, Buffer *buff);

    private:
        Buffer *frameBuffer_ = nullptr;
    };
}

#endif
