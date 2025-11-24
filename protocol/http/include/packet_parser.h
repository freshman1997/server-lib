#ifndef __NET_HTTP_PACKET_PARSER_H__
#define __NET_HTTP_PACKET_PARSER_H__
#include "buffer/buffer_reader.h"

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
        attachment,
        fully,
        too_long // 异常，超过长度
    };

    class HttpPacketParser
    {
    public:
        HttpPacketParser() = default;
        explicit HttpPacketParser(HttpPacket *packet) : packet_(packet)
        {}

        virtual ~HttpPacketParser() = default;

    public:
        virtual bool parse_header(buffer::BufferReader &reader) = 0;

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

        int parse(buffer::BufferReader &reader);

        bool done() const;

        uint32_t get_body_length();

        void set_body_state(BodyState state)
        {
            body_state = state;
        }

        BodyState get_body_state() const
        {
            return body_state;
        }

    protected:
        bool parse_version(buffer::BufferReader &reader, char ending = '\r', char next = '\n');

        bool parse_header_keys(buffer::BufferReader &reader);

        bool parse_new_line(buffer::BufferReader &reader);

        int  parse_body(buffer::BufferReader &reader, uint32_t length) const;

        bool parse_content_disposition(const std::string *val, std::string &originName);

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
        HttpPacket *packet_{};
    };
};

#endif