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

        bool unpack_from(WebSocketConnection *conn, const ::yuan::buffer::ByteBuffer &data);

        bool pack(WebSocketConnection *conn, const ::yuan::buffer::ByteBuffer &buff, uint8_t type);

        bool pack_control(WebSocketConnection *conn, const ::yuan::buffer::ByteBuffer &buff, uint8_t type);

        void update_mask();

        void use_mask(bool use);

    private:
        int read_chunk(ProtoChunk *chunk, ::yuan::buffer::ByteBuffer &buff);

        bool unpack_loop(WebSocketConnection *conn);

        void apply_mask(const ::yuan::buffer::ByteBuffer &data, ::yuan::buffer::ByteBuffer &buff, uint32_t buffSize);

        void apply_mask(::yuan::buffer::ByteBuffer &buff, uint32_t buffSize, uint8_t *mask, uint32_t len);

        bool validate_frame(WebSocketConnection *conn, const ProtoChunk &chunk) const;

        bool append_completed_frame(WebSocketConnection *conn, ProtoChunk &&chunk);

        static bool is_data_frame(const ProtoHead &head);

        static bool is_control_frame(const ProtoHead &head);

        bool pack_header(::yuan::buffer::ByteBuffer &buff, uint8_t type, uint32_t buffSize, bool isEnd, bool isContinueFrame);

        bool pack_frame(const ::yuan::buffer::ByteBuffer &data, ::yuan::buffer::ByteBuffer &buff, std::size_t offset, uint32_t size);

    private:
        bool use_mask_;
        uint8_t mask_[4];
        ::yuan::buffer::ByteBuffer frame_buffer_;
        ProtoChunk pending_frame_;
        ProtoChunk fragmented_message_;
        bool has_fragmented_message_ = false;
    };
}

#endif
