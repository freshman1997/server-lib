#include "packet_parser.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "websocket_connection.h"
#include "websocket_protocol.h"
#include <cassert>

namespace net::websocket
{
    static bool read_chunk(ProtoChunk *chunk, Buffer *buff)
    {
        if (!chunk->has_set_head_) {
            chunk->head_.ctrl_code_.set_ctrl(buff->read_uint8());
            chunk->head_.set_2nd_byte(buff->read_uint8());
            chunk->has_set_head_ = true;

            if (chunk->head_.pay_load_len_ > 125 && chunk->head_.pay_load_len_ <= 126) {
                chunk->head_.extend_pay_load_len_ = buff->read_uint16();
            } else if (chunk->head_.pay_load_len_ > 127) {
                chunk->head_.extend_pay_load_len_ = buff->read_uint64();
            } else {
                chunk->head_.extend_pay_load_len_ = chunk->head_.pay_load_len_;
            }

            if (chunk->head_.mask_) {
                chunk->head_.masking_key_ = buff->read_uint32();
            }
        }

        if (chunk->body_ && chunk->head_.extend_pay_load_len_ > chunk->body_->readable_bytes()) {
            uint32_t remain = chunk->head_.extend_pay_load_len_ - chunk->body_->readable_bytes();
            if (remain <= buff->readable_bytes()) {
                chunk->body_->write_string(buff->peek(), remain);
                buff->add_read_index(remain);
            } else {
                chunk->body_->append_buffer(*buff);
                buff->add_read_index(buff->readable_bytes());
            }
        } else {
            if (!chunk->body_) {
                if (chunk->head_.extend_pay_load_len_ >= buff->readable_bytes()) {
                    chunk->body_ = buff;
                } else {
                    chunk->body_ = BufferedPool::get_instance()->allocate(chunk->head_.extend_pay_load_len_);
                    chunk->body_->write_string(buff->peek(), chunk->head_.extend_pay_load_len_);
                    buff->add_read_index(chunk->head_.extend_pay_load_len_);
                }
            } else {
                return false;
            }
        }

        return true;
    }

    bool WebSocketPacketParser::unpack(WebSocketConnection *conn)
    {
        ProtoHead head;
        conn->get_native_connection()->process_input_data([conn, &head](Buffer *buff) -> bool 
        {
            auto chunks = conn->get_input_chunks();
            if (chunks->empty()) {
                chunks->push_back(ProtoChunk());
            }

            auto *chunk = &chunks->back();
            while (!buff->empty())
            {
                if (!read_chunk(chunk, buff)) {
                    return false;
                }

                if (chunk->is_completed())
                {
                    chunks->push_back(ProtoChunk());
                    chunk = &chunks->back();
                }
            }
            return true;
        }, true);

        return false;
    }

    bool WebSocketPacketParser::pack(WebSocketConnection *conn, Buffer *buff)
    {
        return false;
    }
}