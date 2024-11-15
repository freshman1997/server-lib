#include "websocket_packet_parser.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "websocket_connection.h"
#include "websocket_protocol.h"

#include <cassert>
#include <cstring>

// https://cloud.tencent.com/developer/article/1887095

namespace net::websocket
{
    bool WebSocketPacketParser::read_chunk(ProtoChunk *chunk, Buffer *inputBuff)
    {
        if (frameBuffer_) {
            frameBuffer_->append_buffer(*inputBuff);
        }

        Buffer *buff = frameBuffer_ ? frameBuffer_ : inputBuff;
        if (!chunk->has_set_head_) {
            if (buff->readable_bytes() < 2) {
                return true;
            }

            std::size_t from = buff->get_read_index();

            chunk->head_.ctrl_code_.set_ctrl(buff->read_uint8());
            chunk->head_.set_2nd_byte(buff->read_uint8());

            uint8_t payloadLen = chunk->head_.get_pay_load_len();
            if (payloadLen <= 125) {
                if (buff->readable_bytes() < sizeof(uint32_t)) {
                    buff->reset_read_index(from);
                    frameBuffer_ = buff;
                    return true;
                }
            } else if (payloadLen <= 126) {
                chunk->head_.extend_pay_load_len_ = payloadLen;
                if (buff->readable_bytes() < sizeof(uint16_t) + sizeof(uint32_t)) {
                    buff->reset_read_index(from);
                    frameBuffer_ = buff;
                    return true;
                }
                chunk->head_.extend_pay_load_len_ = buff->read_uint16();
            } else {
                if (buff->readable_bytes() < sizeof(uint64_t) + sizeof(uint32_t)) {
                    buff->reset_read_index(from);
                    frameBuffer_ = buff;
                    return true;
                }
                chunk->head_.extend_pay_load_len_ = buff->read_uint64();
            }

            // set mask key
            for (int i = 0; i < 4; ++i) {
                chunk->head_.masking_key_[i] = buff->read_uint8();
            }

            chunk->has_set_head_ = true;
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
                chunk->body_ = BufferedPool::get_instance()->allocate(chunk->head_.extend_pay_load_len_);
                if (chunk->head_.extend_pay_load_len_ >= buff->readable_bytes()) {
                    chunk->body_->write_string(buff->peek(), buff->readable_bytes());
                    buff->add_read_index(buff->readable_bytes());
                } else {
                    chunk->body_->write_string(buff->peek(), chunk->head_.extend_pay_load_len_);
                    buff->add_read_index(chunk->head_.extend_pay_load_len_);
                }
            } else {
                return false;
            }
        }
        return true;
    }

    bool WebSocketPacketParser::merge_frame(std::vector<ProtoChunk> *chunks)
    {
        if (chunks->size() <= 1) {
            ProtoChunk *lastChunk = &chunks->at(0);
            if (lastChunk->head_.mask_ & 0x01) {
                frame_decode(lastChunk->body_, lastChunk->head_.masking_key_, 4);
            }
            return true;
        }

        ProtoChunk *lastChunk = &chunks->at(chunks->size() - 2);
        if (!lastChunk->head_.is_fin() || !lastChunk->body_) {
            return false;
        }

        ProtoChunk *chunk = &chunks->back();
        if (!chunk->body_) {
            return false;
        }

        lastChunk->head_.extend_pay_load_len_ += chunk->head_.extend_pay_load_len_;
        lastChunk->body_->append_buffer(*chunk->body_);

        if (lastChunk->head_.mask_ & 0x01) {
            frame_decode(lastChunk->body_, lastChunk->head_.masking_key_, 4);
        }

        return true;
    }

    void WebSocketPacketParser::frame_decode(Buffer *buff, uint8_t *mask, uint32_t len)
    {
        std::size_t sz = buff->readable_bytes();
        char *p = buff->peek_for();
        char *end = p + sz;
        for (int i = 0; i < sz && p <= end; ++i, ++p) {
            *p = *p ^ mask[i % len];
        }
    }

    bool WebSocketPacketParser::unpack(WebSocketConnection *conn)
    {
        bool res = true;
        conn->get_native_connection()->process_input_data([conn, this, &res](Buffer *buff) -> bool 
        {
            auto chunks = conn->get_input_chunks();
            if (chunks->empty()) {
                chunks->push_back(ProtoChunk());
            }

            auto *chunk = &chunks->back();
            while (!buff->empty())
            {
                if (!read_chunk(chunk, buff)) {
                    res = false;
                    return false;
                }

                if (chunk->is_completed()) {
                    if (!merge_frame(chunks)) {
                        res = false;
                        return false;
                    }

                    if (chunk->head_.is_fin()) {
                        chunks->push_back(ProtoChunk());
                        chunk = &chunks->back();
                    } else {
                        BufferedPool::get_instance()->free(chunk->body_);

                        memset(chunk, 1, sizeof(*chunk));
                        chunk->has_set_head_ = false;
                        chunk->body_ = nullptr;

                        if (!chunks->back().head_.is_continue_frame() && chunks->back().is_completed()) {
                            chunks->push_back(ProtoChunk());
                            chunk = &chunks->back();
                        }
                    }
                }
            }
            return true;
        }, false);

        return res;
    }

    Buffer * WebSocketPacketParser::pack_header(WebSocketConnection *conn, Buffer *buff)
    {
        Buffer *data = BufferedPool::get_instance()->allocate(100);
        ProtoHead head;
        return nullptr;
    }

    bool WebSocketPacketParser::pack_frame(WebSocketConnection *conn, Buffer *buff)
    {
        auto buffers = conn->get_output_buffers();
        buffers->push_back(buff);
        return true;
    }

    bool WebSocketPacketParser::pack(WebSocketConnection *conn, Buffer *buff)
    {
        auto buffers = conn->get_output_buffers();
        while (buff->empty()) {
            if (!pack_header(conn, buff)) {
                return false;
            }
        }
        return pack_frame(conn, buff);
    }
}
