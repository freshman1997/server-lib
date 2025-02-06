#ifndef __NET_HTTP_PACKET_PARSER_H__
#define __NET_HTTP_PACKET_PARSER_H__
#include "buffer/buffer.h"

namespace yuan::net::http
{
    class HttpPacket;

    enum class HeaderState : int
    {
        // request
        too_long = -1,                      // 异常，超过长度

        init = 0,
        metohd,                             // 方法
        method_gap,                         // 方法接下来的空格
        url,                                // url
        url_gap,                            // url 接下来的空格
        version,                            // 版本信息
        version_newline,                    // 换行

        // response
        version_gap,                        // 版本信息 接下来的空格
        header_status,                      // 响应状态
        header_status_gap,                  // 响应状态接下来的空格
        header_status_desc,                 // 响应状态对应的描述， 如 bad request
        header_status_desc_gap,

        // header
        header_key,                         // 头部key
        header_value,                       // 值

        header_end_lines,                   // 最后换行

    };

    enum class BodyState : int
    {
        init = 0,
        empty,
        partial,
        fully,
        too_long // 异常，超过长度
    };

    class HttpPacketParser
    {
    public:
        HttpPacketParser() {}
        HttpPacketParser(HttpPacket *packet) : packet_(packet) 
        {}

        virtual ~HttpPacketParser() {}

    public:
        virtual bool parse_header(buffer::Buffer &buff) = 0;

    public:
        void set_ptr(HttpPacket *packet)
        {
            this->packet_ = packet;
        }

        void reset() 
        {
            header_state = HeaderState::init;
            body_state = BodyState::init;
        }

        int parse(buffer::Buffer &buff);

        bool done() const
        {
            return is_header_done() && is_body_done();
        }

        uint32_t get_body_length();

    protected:
        bool parse_version(buffer::Buffer &buff, char ending = '\r', char next = '\n');

        bool parse_header_keys(buffer::Buffer &buff);

        bool parse_new_line(buffer::Buffer &buff);

        int  parse_body(buffer::Buffer &buff, uint32_t length);

        bool is_header_done() const 
        {
            return header_state == HeaderState::header_end_lines;
        }

        bool is_body_done() const 
        {
            return body_state == BodyState::fully || body_state == BodyState::empty;
        }

    protected:
        HeaderState header_state = HeaderState::init;
        BodyState body_state = BodyState::init;
        HttpPacket *packet_;
    };
};

#endif