#ifndef __NET_WEBSOCKET_COMMON_WEBSOCKET_PROTOCOL_H__
#define __NET_WEBSOCKET_COMMON_WEBSOCKET_PROTOCOL_H__
#include "buffer/byte_buffer.h"
#include <cstdint>
#include <limits>

namespace yuan::net::websocket
{
    constexpr uint32_t PACKET_MAX_BYTE = 1024 * 1024;

    enum class WorkMode : uint8_t {
        client_,
        server_
    };

    enum class OpCodeType : uint8_t {
        type_continue_frame = 0x00,
        type_text_frame = 0x01,
        type_binary_frame = 0x02,
        type_close_frame = 0x08,
        type_ping_frame = 0x09,
        type_pong_frame = 0x0a,
    };

    struct ProtoHead
    {
        struct ControlCode
        {
            unsigned char fin_ : 1;
            unsigned char rev_1_ : 1;
            unsigned char rev_2_ : 1;
            unsigned char rev_3_ : 1;
            unsigned char opcode_ : 4;

            void set_ctrl(unsigned char ch)
            {
                fin_ = (ch >> 7) & 0x01;
                rev_1_ = (ch >> 6) & 0x01;
                rev_2_ = (ch >> 5) & 0x01;
                rev_3_ = (ch >> 4) & 0x01;
                opcode_ = ch & 0x0f;
            }

        } ctrl_code_;

        unsigned char mask_ : 1;

        unsigned char pay_load_len_ : 7;

        uint8_t masking_key_[4];

        uint64_t extend_pay_load_len_;

        ProtoHead()
            : ctrl_code_(), mask_(0), pay_load_len_(0), masking_key_{0, 0, 0, 0}, extend_pay_load_len_(0)
        {
        }

        void set_2nd_byte(unsigned char byte)
        {
            mask_ = (byte >> 7) & 0x01;
            pay_load_len_ = byte & 0x7f;
        }

        uint8_t get_pay_load_len() const
        {
            return pay_load_len_ & 0x7f;
        }

        bool is_fin() const
        {
            return ctrl_code_.fin_ & 0x01;
        }

        bool need_mask() const
        {
            return mask_ & 0x01;
        }

        bool is_continue_frame() const
        {
            return (ctrl_code_.opcode_ & 0x0f) == (uint8_t)OpCodeType::type_continue_frame;
        }

        bool is_text_frame() const
        {
            return (ctrl_code_.opcode_ & 0x0f) == (uint8_t)OpCodeType::type_text_frame;
        }

        bool is_binary_frame() const
        {
            return (ctrl_code_.opcode_ & 0x0f) == (uint8_t)OpCodeType::type_binary_frame;
        }

        bool is_close_frame() const
        {
            return (ctrl_code_.opcode_ & 0x0f) == (uint8_t)OpCodeType::type_close_frame;
        }

        bool is_ping_frame() const
        {
            return (ctrl_code_.opcode_ & 0x0f) == (uint8_t)OpCodeType::type_ping_frame;
        }

        bool is_pong_frame() const
        {
            return (ctrl_code_.opcode_ & 0x0f) == (uint8_t)OpCodeType::type_pong_frame;
        }

        bool has_rsv() const
        {
            return (ctrl_code_.rev_1_ | ctrl_code_.rev_2_ | ctrl_code_.rev_3_) != 0;
        }

        uint8_t opcode() const
        {
            return ctrl_code_.opcode_ & 0x0f;
        }
    };

    struct ProtoChunk
    {
        ProtoHead head_;
        bool has_set_head_ = false;
        ::yuan::buffer::ByteBuffer body_;

        bool is_completed() const
        {
            if (!has_set_head_) {
                return false;
            }

            if (head_.extend_pay_load_len_ > 0) {
                return body_.readable_bytes() >= head_.extend_pay_load_len_;
            }

            return true;
        }

        static int calc_head_size(uint32_t dataSize, bool setMask = true)
        {
            int res = 2;
            if (dataSize <= 125) {
            } else if (dataSize <= 0xffff) {
                res += 2;
            } else if (dataSize <= PACKET_MAX_BYTE) {
                res += 8;
            } else {
                return -1;
            }

            if (setMask) {
                res += 4;
            }

            return res;
        }
    };
}

#endif
