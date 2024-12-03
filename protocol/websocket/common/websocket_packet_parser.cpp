#include "websocket_packet_parser.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "websocket_connection.h"
#include "websocket_protocol.h"

#include <cassert>
#include <cstring>
#include <random>

namespace net::websocket
{
    WebSocketPacketParser::WebSocketPacketParser() : use_mask_(false), frame_buffer_(nullptr)
    {
        if (use_mask_) {
            update_mask();
        }
    }

    WebSocketPacketParser::~WebSocketPacketParser()
    {
        if (frame_buffer_) {
            BufferedPool::get_instance()->free(frame_buffer_);
            frame_buffer_ = nullptr;
        }
    }

    int WebSocketPacketParser::read_chunk(ProtoChunk *chunk, Buffer *buff)
    {
        std::size_t from = buff->get_read_index();
        if (!chunk->has_set_head_) {
            if (buff->readable_bytes() < 2) {
                buff->reset_read_index(from);
                return 1;
            }

            chunk->head_.ctrl_code_.set_ctrl(buff->read_uint8());
            chunk->head_.set_2nd_byte(buff->read_uint8());

            uint32_t payloadLen = chunk->head_.get_pay_load_len();
            if (payloadLen <= 125) {
                chunk->head_.extend_pay_load_len_ = payloadLen;
            } else if (payloadLen <= 126) {
                if (buff->readable_bytes() < 2) {
                    buff->reset_read_index(from);
                    return 1;
                }
                chunk->head_.extend_pay_load_len_ = buff->read_uint16() & 0xffff;
            } else {
                if (buff->readable_bytes() < 8) {
                    buff->reset_read_index(from);
                    return 1;
                }
                chunk->head_.extend_pay_load_len_ = buff->read_uint64();
            }

            if (chunk->head_.extend_pay_load_len_ > PACKET_MAX_BYTE) {
                return -1;
            }

            if (buff->readable_bytes() < chunk->head_.extend_pay_load_len_) {
                buff->reset_read_index(from);
                return 1;
            }

            // set mask key
            if (chunk->head_.need_mask()) {
                if (buff->readable_bytes() < 4) {
                    buff->reset_read_index(from);
                    return 1;
                }

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
        } else if (chunk->head_.extend_pay_load_len_ > 0) {
            if (!chunk->body_) {
                chunk->body_ = BufferedPool::get_instance()->allocate(chunk->head_.extend_pay_load_len_);
            }

            if (chunk->head_.extend_pay_load_len_ >= buff->readable_bytes()) {
                chunk->body_->append_buffer(*buff);
                buff->add_read_index(buff->readable_bytes());
            } else {
                chunk->body_->write_string(buff->peek(), chunk->head_.extend_pay_load_len_);
                buff->add_read_index(chunk->head_.extend_pay_load_len_);
            }
        }

        return 0;
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
            frame_buffer_ = get_frame_buffer();
            auto chunks = conn->get_input_chunks();
            if (chunks->empty() || chunks->back().head_.is_fin()) {
                chunks->push_back(ProtoChunk());
            }

            auto pc = &chunks->back();
            while (true) {
                int unpackRes = -1;
                ProtoChunk chunk;
                if (frame_buffer_->empty()) {
                    unpackRes = read_chunk(&chunk, buff);
                } else {
                    if (!buff->empty()) {
                        frame_buffer_->append_buffer(*buff);
                        buff->set_read_index(buff->readable_bytes());
                    }
                    unpackRes = read_chunk(&chunk, frame_buffer_);
                }

                if (unpackRes < 0) {
                    res = false;
                    return res;
                }

                if (unpackRes == 1) {
                    if (!buff->empty()) {
                        frame_buffer_->append_buffer(*buff);
                    }
                    return true;
                }

                bool merge = false;
                if (pc->has_set_head_) {
                    if (!pc->head_.is_fin()) {
                        if (!chunk.head_.is_fin() && (!chunk.head_.is_continue_frame() || !pc->body_ || !chunk.body_)) {
                            res = false;
                            return res;
                        }
                    } else {
                        res = false;
                        return res;
                    }
                    merge = true;
                } else {
                    *pc = chunk;
                    if (!pc->head_.is_fin()) {
                        if (!pc->head_.is_continue_frame() && !pc->head_.is_text_frame() && !pc->head_.is_binary_frame()) {
                            res = false;
                            return res;
                        }
                    }
                }

                // 到这里应该是已经读完包体的
                if (chunk.body_) {
                    assert(chunk.head_.extend_pay_load_len_ == chunk.body_->readable_bytes());
                    if (chunk.head_.need_mask()) {
                        apply_mask(chunk.body_, (uint32_t)chunk.body_->readable_bytes(), chunk.head_.masking_key_, 4);
                    }
                }

                if (merge) {
                    // 合并
                    pc->head_.extend_pay_load_len_ += chunk.head_.extend_pay_load_len_;
                    // TODO 包体优化，减少拷贝
                    pc->body_->append_buffer(*chunk.body_);
                    BufferedPool::get_instance()->free(chunk.body_);
                }

                if (pc->head_.extend_pay_load_len_ > PACKET_MAX_BYTE) {
                    res = false;
                    return res;
                }

                if (chunk.head_.is_fin()) {
                    chunks->push_back(ProtoChunk());
                    pc = &chunks->back();
                }

                frame_buffer_->shink_to_fit();

                if (frame_buffer_->empty() && buff->empty()) {
                    break;
                }
            }
            return true;
        });

        auto chunks = conn->get_input_chunks();
        if (chunks->size() >= 1 && !chunks->back().has_set_head_ && chunks->back().body_ == nullptr) {
            chunks->pop_back();
        }

        frame_buffer_->shink_to_fit();

        return res;
    }

    bool WebSocketPacketParser::pack_header(Buffer *buff, uint8_t type, uint32_t buffSize, bool isEnd, bool isContinueFrame)
    {
        if (buff->writable_size() < 2) {
            return false;
        }

        uint8_t head1 = 0x00;
        if (isEnd) {
            head1 = 0b10000000;
        }

        // 第一帧才设置，延续帧 opcode 都是0x00
        if (!isContinueFrame) {
            if (type == (uint8_t)OpCodeType::type_text_frame) {
                head1 |= (uint8_t)OpCodeType::type_text_frame & 0xff;
            } else if (type == (uint8_t)OpCodeType::type_binary_frame) {
                head1 |= (uint8_t)OpCodeType::type_binary_frame & 0xff;
            } else {
                return false;
            }
        }

        uint8_t head2 = 0;
        if (use_mask_) {
            head2 = 0b10000000;
        }

        if (buffSize <= 0x7f) {
            head2 |= (uint8_t)buffSize;
        } else if (buffSize <= 65535) {
            head2 |= 0x7f - 1;
        } else if (buffSize <= PACKET_MAX_BYTE) {
            head2 |= 0x7f;
        } else {
            return false;
        }

        buff->write_uint8(head1);
        buff->write_uint8(head2);

        if ((head2 & 0x7f) == 126) {
            if (buff->writable_size() < 2) {
                return false;
            }
            buff->write_uint16((uint16_t)buffSize);
        } else if ((head2 & 0x7f) == 127) {
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
            int headSize = ProtoChunk::calc_head_size(buffSize, use_mask_);
            if (headSize < 0) {
                return false;
            }

            Buffer *buff = BufferedPool::get_instance()->allocate(buffSize + headSize);
            if (!pack_header(buff, type, buffSize, i + 1 >= sz, i > 0)) {
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

    void WebSocketPacketParser::use_mask(bool use)
    {
        use_mask_ = use;
        if (use_mask_) {
            update_mask();
        }
    }

    Buffer * WebSocketPacketParser::get_frame_buffer()
    {
        if (!frame_buffer_) {
            frame_buffer_ = BufferedPool::get_instance()->allocate();
        }
        return frame_buffer_;
    }
}
