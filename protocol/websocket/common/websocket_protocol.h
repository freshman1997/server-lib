#ifndef __NET_WEBSOCKET_COMMON_WEBSOCKET_PROTOCOL_H__
#define __NET_WEBSOCKET_COMMON_WEBSOCKET_PROTOCOL_H__
#include "buffer/buffer.h"
#include <cstdint>

namespace net::websocket 
{
    struct ProtoHead
    {
        struct ControlCode
        {
            char fin_ : 1;
            char rev_1_ : 1;
            char rev_2_ : 1;
            char rev_3_ : 1;
            char opcode_ : 4;

            void set_ctrl(unsigned char ch)
            {
                fin_ = ch & (7 << 1);
                rev_1_ = ch & (6 << 1);
                rev_2_ = ch & (5 << 1);
                rev_3_ = ch & (4 << 1);
                opcode_ = (4 << ch) >> 4;
            }

        } ctrl_code_;

        char mask_ : 1;
        
        char pay_load_len_ : 7;

        uint32_t masking_key_;

        uint64_t extend_pay_load_len_;

        void set_2nd_byte(unsigned char byte)
        {
            mask_ = byte >> 7;
            pay_load_len_ = (1 << byte) >> 1;
        }

        bool is_fin()
        {
            return ctrl_code_.fin_;
        }

        bool is_continue_frame()
        {
            return ctrl_code_.opcode_ == 0x00;
        }

        bool is_text_frame()
        {
            return ctrl_code_.opcode_ == 0x01;
        }

        bool is_binary_frame()
        {
            return ctrl_code_.opcode_ == 0x02;
        }

        bool is_close_frame()
        {
            return ctrl_code_.opcode_ == 0x08;
        }

        bool is_ping_frame()
        {
            return ctrl_code_.opcode_ == 0x09;
        }

        bool is_pong_frame()
        {
            return ctrl_code_.opcode_ == 0x0a;
        }
    };

    struct ProtoChunk
    {
        ProtoHead head_;
        bool has_set_head_ = false;
        Buffer * body_ = nullptr;

        bool is_completed()
        {
            if (head_.extend_pay_load_len_ > 0)
            {
                return body_ ? body_->readable_bytes() >= head_.extend_pay_load_len_ : false;
            }

            return true;
        }
    };
}

#endif