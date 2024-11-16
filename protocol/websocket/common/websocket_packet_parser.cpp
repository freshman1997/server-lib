#include "websocket_packet_parser.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "websocket_connection.h"
#include "websocket_protocol.h"

#include <cassert>
#include <cstring>
#include <limits>
#include <random>

// https://cloud.tencent.com/developer/article/1887095

namespace net::websocket
{
    WebSocketPacketParser::WebSocketPacketParser() : use_mask_(false)
    {
        if (use_mask_) {
            update_mask();
        }
    }

    bool WebSocketPacketParser::read_chunk(ProtoChunk *chunk, Buffer *inputBuff)
    {
        if (!frame_buffer_.empty()) {
            frame_buffer_.append_buffer(*inputBuff);
        }


        Buffer *buff = frame_buffer_.empty() ? inputBuff : &frame_buffer_;
        std::size_t from = buff->get_read_index(), fromSize = frame_buffer_.readable_bytes();
        if (!chunk->has_set_head_) {
            if (buff->readable_bytes() < 2) {
                return true;
            }

            chunk->head_.ctrl_code_.set_ctrl(buff->read_uint8());
            chunk->head_.set_2nd_byte(buff->read_uint8());

            uint8_t payloadLen = chunk->head_.get_pay_load_len();
            if (payloadLen <= 125) {
                if (buff->readable_bytes() < sizeof(uint32_t)) {
                    buff->reset_read_index(from);
                    if (fromSize == 0) {
                        frame_buffer_.append_buffer(*inputBuff);
                    }
                    return true;
                }
                chunk->head_.extend_pay_load_len_ = payloadLen;
            } else if (payloadLen <= 126) {
                if (buff->readable_bytes() < sizeof(uint16_t) + sizeof(uint32_t)) {
                    buff->reset_read_index(from);
                    if (fromSize == 0) {
                        frame_buffer_.append_buffer(*inputBuff);
                    }
                    return true;
                }
                chunk->head_.extend_pay_load_len_ = buff->read_uint16();
            } else {
                if (buff->readable_bytes() < sizeof(uint64_t) + sizeof(uint32_t)) {
                    buff->reset_read_index(from);
                    if (fromSize == 0) {
                        frame_buffer_.append_buffer(*inputBuff);
                    }
                    return true;
                }
                chunk->head_.extend_pay_load_len_ = buff->read_uint64();
            }

            // set mask key
            if (chunk->head_.mask_ & 0x01) {
                for (int i = 0; i < 4; ++i) {
                    chunk->head_.masking_key_[i] = buff->read_uint8();
                }
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

        if (fromSize > 0) {
            inputBuff->add_read_index(fromSize - frame_buffer_.readable_bytes());
        }

        if (frame_buffer_.empty()) {
            frame_buffer_.reset();
        }

        return true;
    }

    bool WebSocketPacketParser::merge_frame(std::vector<ProtoChunk> *chunks)
    {
        ProtoChunk *lastChunk = nullptr;
        if (chunks->size() > 1) {
            lastChunk = &chunks->at(chunks->size() - 2);
            if (!lastChunk->head_.is_fin() || !lastChunk->body_) {
                return false;
            }

            ProtoChunk *chunk = &chunks->back();
            if (!chunk->body_) {
                return false;
            }

            lastChunk->head_.extend_pay_load_len_ += chunk->head_.extend_pay_load_len_;
            lastChunk->body_->append_buffer(*chunk->body_);
        } else {
            lastChunk = &chunks->at(0);
        }

        if (lastChunk->head_.mask_ & 0x01) {
            apply_mask(lastChunk->body_, lastChunk->body_->readable_bytes(), lastChunk->head_.masking_key_, 4);
        }

        return true;
    }

    void WebSocketPacketParser::apply_mask(Buffer *buff, uint32_t buffSize, uint8_t *mask, uint32_t len)
    {
        char *p = buff->peek_for();
        char *end = p + buffSize;
        for (int i = 0; i < buffSize && p <= end; ++i, ++p) {
            *p = *p ^ mask[i % len];
        }
    }

    void WebSocketPacketParser::apply_mask(Buffer *data, Buffer *buff, uint32_t buffSize)
    {
        const char *p = data->peek();
        const char *end = p + buffSize;
        for (int i = 0; i < buffSize && p <= end; ++i, ++p) {
            buff->write_uint8(*p ^ mask_[i % 4]);
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

                    if (chunk->body_) {
                        assert(chunk->head_.extend_pay_load_len_ == chunk->body_->readable_bytes());
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

    bool WebSocketPacketParser::pack_header(Buffer *buff, uint8_t type, uint32_t buffSize, bool isEnd)
    {
        if (buff->writable_size() < 2) {
            return false;
        }

        uint8_t head1 = 0;
        if (isEnd) {
            head1 = 0b10000000;
        }

        if (type == (uint8_t)OpCodeType::type_text_frame) {
            head1 |= (uint8_t)OpCodeType::type_text_frame;
        } else if (type == (uint8_t)OpCodeType::type_binary_frame) {
            head1 |= (uint8_t)OpCodeType::type_binary_frame;
        } else {
            return false;
        }

        uint8_t head2 = 0;
        if (buffSize <= 0x7f) {
            head2 |= (uint8_t)buffSize;
        } else if (buffSize <= std::numeric_limits<short>::max()) {
            head2 = 0b11111110;
        } else if (buffSize <= PACKET_MAX_BYTE) {
            head2 = 0xff;
        } else {
            return false;
        }

        buff->write_uint8(head1);
        buff->write_uint8(head2);

        if (head2 == 126) {
            if (buff->writable_size() < 2) {
                return false;
            }
            buff->write_uint16((uint16_t)buffSize);
        } else if (head2 == 127) {
            if (buff->writable_size() < 8) {
                return false;
            }
            buff->write_uint64(buffSize);
        }

        if (use_mask_) {
            if (buff->writable_size() < 4) {
                return false;
            }

            for (int i = 0; i < 4; ++i) {
                buff->write_uint8(mask_[i]);
            }
        }

        return true;
    }

    bool WebSocketPacketParser::pack_frame(Buffer *data, Buffer *buff, uint32_t size)
    {
        if (buff->writable_size() < size) {
            return false;
        }

        if (use_mask_) {
            apply_mask(data, buff, size);
        } else {
            buff->write_string(data->peek(), size);
        }

        data->add_read_index(size);

        return true;
    }

    bool WebSocketPacketParser::pack(WebSocketConnection *conn, Buffer *data, uint8_t type)
    {
        auto buffers = conn->get_output_buffers();
        uint32_t sz = data->readable_bytes() / PACKET_MAX_BYTE + 1, buffSize = 0;
        for (int i = 0; i < sz; ++i) {
            buffSize = data->readable_bytes() > PACKET_MAX_BYTE ? PACKET_MAX_BYTE : data->readable_bytes();
            int headSize = ProtoChunk::calc_head_size(buffSize);
            if (headSize < 0) {
                return false;
            }

            Buffer *buff = BufferedPool::get_instance()->allocate(buffSize + headSize);
            if (!pack_header(buff, type, buffSize, i + 1 >= sz)) {
                BufferedPool::get_instance()->free(buff);
                return false;
            }

            if (!pack_frame(data, buff, buffSize)) {
                BufferedPool::get_instance()->free(buff);
                return false;
            }

            buffers->push_back(buff);
        }
        return true;
    }

    static uint32_t generateMask() 
    {
        static std::mt19937_64 generator(std::random_device{}());
        static std::uniform_int_distribution<uint32_t> distribution;
        return distribution(generator);
    }

    void WebSocketPacketParser::update_mask()
    {
        for (int i = 0; i < 4; ++i) {
            mask_[i] = generateMask() % 0xff;
        }
    }
}
