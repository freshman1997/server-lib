#include "websocket_packet_parser.h"
#include "websocket_connection.h"

#include <cassert>
#include <cstddef>
#include <random>

namespace yuan::net::websocket
{
    WebSocketPacketParser::WebSocketPacketParser()
        : use_mask_(false)
    {
        if (use_mask_) {
            update_mask();
        }
    }

    WebSocketPacketParser::~WebSocketPacketParser() = default;

    int WebSocketPacketParser::read_chunk(ProtoChunk * chunk, yuan::buffer::ByteBuffer & buff)
    {
        const std::size_t from = buff.read_offset();
        if (!chunk->has_set_head_) {
            if (buff.readable_bytes() < 2) {
                buff.set_read_offset(from);
                return 1;
            }

            chunk->head_.ctrl_code_.set_ctrl(buff.read_u8());
            chunk->head_.set_2nd_byte(buff.read_u8());

            const uint32_t payloadLen = chunk->head_.get_pay_load_len();
            if (payloadLen <= 125) {
                chunk->head_.extend_pay_load_len_ = payloadLen;
            } else if (payloadLen <= 126) {
                if (buff.readable_bytes() < 2) {
                    buff.set_read_offset(from);
                    return 1;
                }
                chunk->head_.extend_pay_load_len_ = buff.read_u16() & 0xffff;
            } else {
                if (buff.readable_bytes() < 8) {
                    buff.set_read_offset(from);
                    return 1;
                }
                chunk->head_.extend_pay_load_len_ = buff.read_u64();
            }

            if (chunk->head_.extend_pay_load_len_ > PACKET_MAX_BYTE) {
                return -1;
            }

            if (chunk->head_.need_mask()) {
                if (buff.readable_bytes() < 4) {
                    buff.set_read_offset(from);
                    return 1;
                }

                for (int i = 0; i < 4; ++i) {
                    chunk->head_.masking_key_[i] = buff.read_u8();
                }
            }

            chunk->has_set_head_ = true;
        }

        const auto alreadyRead = chunk->body_.readable_bytes();
        if (chunk->head_.extend_pay_load_len_ > alreadyRead) {
            const std::size_t remain = static_cast<std::size_t>(chunk->head_.extend_pay_load_len_ - alreadyRead);
            const std::size_t toRead = std::min<size_t>(remain, buff.readable_bytes());
            if (toRead > 0) {
                chunk->body_.append(buff.read_ptr(), toRead);
                buff.consume(toRead);
            }
        }

        return 0;
    }

    void WebSocketPacketParser::apply_mask(yuan::buffer::ByteBuffer & buff, uint32_t buffSize, uint8_t * mask, uint32_t len)
    {
        auto *p = buff.read_ptr();
        auto *end = p + buffSize;
        for (int i = 0; i < static_cast<int>(buffSize) && p < end; ++i, ++p) {
            *p = *p ^ mask[i % len];
        }
    }

    void WebSocketPacketParser::apply_mask(const yuan::buffer::ByteBuffer & data, yuan::buffer::ByteBuffer & buff, uint32_t buffSize)
    {
        const auto *p = data.read_ptr();
        const auto *end = p + buffSize;
        for (int i = 0; i < static_cast<int>(buffSize) && p < end; ++i, ++p) {
            buff.append_u8(static_cast<uint8_t>(*p) ^ mask_[i % 4]);
        }
    }

    bool WebSocketPacketParser::unpack_loop(WebSocketConnection * conn)
    {
        auto chunks = conn->get_input_chunks();
        if (chunks->empty() || chunks->back().head_.is_fin()) {
            chunks->push_back(ProtoChunk());
        }

        auto *pc = &chunks->back();
        while (true) {
            ProtoChunk chunk;
            const int unpackRes = read_chunk(&chunk, frame_buffer_);
            if (unpackRes < 0) {
                return false;
            }

            if (unpackRes == 1) {
                frame_buffer_.compact();
                return true;
            }

            bool merge = false;
            if (pc->has_set_head_) {
                if (!pc->head_.is_fin()) {
                    if (!chunk.head_.is_fin() && (!chunk.head_.is_continue_frame() || chunk.body_.empty())) {
                        return false;
                    }
                } else {
                    return false;
                }
                merge = true;
            } else {
                *pc = std::move(chunk);
                if (!pc->head_.is_fin()) {
                    if (!pc->head_.is_continue_frame() && !pc->head_.is_text_frame() && !pc->head_.is_binary_frame()) {
                        return false;
                    }
                }
            }

            if (!merge && !pc->body_.empty() && pc->head_.need_mask()) {
                apply_mask(pc->body_, static_cast<uint32_t>(pc->body_.readable_bytes()), pc->head_.masking_key_, 4);
            }

            if (merge) {
                if (!chunk.body_.empty() && chunk.head_.need_mask()) {
                    apply_mask(chunk.body_, static_cast<uint32_t>(chunk.body_.readable_bytes()), chunk.head_.masking_key_, 4);
                }
                pc->head_.ctrl_code_.fin_ = chunk.head_.ctrl_code_.fin_;
                pc->head_.extend_pay_load_len_ += chunk.head_.extend_pay_load_len_;
                pc->body_.append(chunk.body_);
            }

            if (pc->head_.extend_pay_load_len_ > PACKET_MAX_BYTE) {
                return false;
            }

            if (pc->head_.is_fin() && pc->is_completed()) {
                chunks->push_back(ProtoChunk());
                pc = &chunks->back();
            }

            frame_buffer_.compact();
            if (frame_buffer_.empty()) {
                break;
            }
        }

        if (!chunks->empty() && !chunks->back().has_set_head_ && chunks->back().body_.empty()) {
            chunks->pop_back();
        }

        frame_buffer_.compact();
        return true;
    }

    bool WebSocketPacketParser::unpack_from(WebSocketConnection * conn, const ::yuan::buffer::ByteBuffer & data)
    {
        if (!data.empty()) {
            frame_buffer_.append(data);
        }

        return unpack_loop(conn);
    }

    bool WebSocketPacketParser::unpack(WebSocketConnection * conn)
    {
        auto input = conn->get_native_connection()->take_input_byte_buffer();
        if (!input.empty()) {
            frame_buffer_.append(input);
        }

        return unpack_loop(conn);
    }

    bool WebSocketPacketParser::pack_header(yuan::buffer::ByteBuffer & buff, uint8_t type, uint32_t buffSize, bool isEnd, bool isContinueFrame)
    {
        uint8_t head1 = 0x00;
        if (isEnd) {
            head1 = 0b10000000;
        }

        if (!isContinueFrame) {
            if (type == static_cast<uint8_t>(OpCodeType::type_text_frame) ||
                type == static_cast<uint8_t>(OpCodeType::type_binary_frame) ||
                type == static_cast<uint8_t>(OpCodeType::type_close_frame) ||
                type == static_cast<uint8_t>(OpCodeType::type_ping_frame) ||
                type == static_cast<uint8_t>(OpCodeType::type_pong_frame)) {
                head1 |= type & 0x0f;
            } else {
                return false;
            }
        }

        uint8_t head2 = 0;
        if (use_mask_) {
            head2 = 0b10000000;
        }

        if (buffSize <= 125) {
            head2 |= static_cast<uint8_t>(buffSize);
        } else if (buffSize <= 65535) {
            head2 |= 126;
        } else if (buffSize <= PACKET_MAX_BYTE) {
            head2 |= 127;
        } else {
            return false;
        }

        buff.append_u8(head1);
        buff.append_u8(head2);

        if ((head2 & 0x7f) == 126) {
            buff.append_u16(static_cast<uint16_t>(buffSize));
        } else if ((head2 & 0x7f) == 127) {
            buff.append_u64(buffSize);
        }

        if (use_mask_) {
            for (int i = 0; i < 4; ++i) {
                buff.append_u8(mask_[i]);
            }
        }

        return true;
    }

    bool WebSocketPacketParser::pack_frame(const yuan::buffer::ByteBuffer & data, yuan::buffer::ByteBuffer & buff, std::size_t offset, uint32_t size)
    {
        if (offset + size > data.readable_bytes()) {
            return false;
        }

        const auto *frameStart = data.read_ptr() + offset;
        if (use_mask_) {
            yuan::buffer::ByteBuffer chunk(frameStart, size);
            apply_mask(chunk, size, mask_, 4);
            buff.append(chunk);
        } else {
            buff.append(frameStart, size);
        }

        return true;
    }

    bool WebSocketPacketParser::pack(WebSocketConnection * conn, const yuan::buffer::ByteBuffer & data, uint8_t type)
    {
        auto buffers = conn->get_output_buffers();
        uint32_t sz = static_cast<uint32_t>(data.readable_bytes() / PACKET_MAX_BYTE + 1);
        uint32_t buffSize = 0;
        std::size_t offset = 0;
        for (uint32_t i = 0; i < sz; ++i) {
            const auto remain = data.readable_bytes() - offset;
            buffSize = remain > PACKET_MAX_BYTE ? PACKET_MAX_BYTE : static_cast<uint32_t>(remain);
            const int headSize = ProtoChunk::calc_head_size(buffSize, use_mask_);
            if (headSize < 0) {
                return false;
            }

            yuan::buffer::ByteBuffer buff(buffSize + headSize);
            if (!pack_header(buff, type, buffSize, i + 1 >= sz, i > 0)) {
                return false;
            }

            if (!pack_frame(data, buff, offset, buffSize)) {
                return false;
            }

            buffers->push_back(std::move(buff));
            offset += buffSize;
        }
        return true;
    }

    bool WebSocketPacketParser::pack_control(WebSocketConnection * conn, const yuan::buffer::ByteBuffer & data, uint8_t type)
    {
        if (type != static_cast<uint8_t>(OpCodeType::type_close_frame) &&
            type != static_cast<uint8_t>(OpCodeType::type_ping_frame) &&
            type != static_cast<uint8_t>(OpCodeType::type_pong_frame)) {
            return false;
        }

        const uint32_t payloadSize = static_cast<uint32_t>(data.readable_bytes());
        if (payloadSize > 125) {
            return false;
        }

        auto buffers = conn->get_output_buffers();

        const int headSize = ProtoChunk::calc_head_size(payloadSize, use_mask_);
        if (headSize < 0) {
            return false;
        }

        yuan::buffer::ByteBuffer buff(payloadSize + headSize);
        if (!pack_header(buff, type, payloadSize, true, false)) {
            return false;
        }

        if (payloadSize > 0) {
            if (!pack_frame(data, buff, 0, payloadSize)) {
                return false;
            }
        }

        buffers->push_back(std::move(buff));
        return true;
    }

    static uint32_t generateMask()
    {
        static thread_local std::mt19937_64 generator(std::random_device {}());
        static thread_local std::uniform_int_distribution<uint32_t> distribution;
        return distribution(generator);
    }

    void WebSocketPacketParser::update_mask()
    {
        for (int i = 0; i < 4; ++i) {
            mask_[i] = generateMask() % 256;
        }
    }

    void WebSocketPacketParser::use_mask(bool use)
    {
        use_mask_ = use;
        if (use_mask_) {
            update_mask();
        }
    }
}
